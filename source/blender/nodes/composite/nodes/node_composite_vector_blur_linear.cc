/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_composite_vector_blur_linear.hh"
#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "COM_algorithm_jump_flooding.hh"
#include "COM_context.hh"
#include "COM_utilities.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include <cstdint>

namespace blender::nodes::node_composite_vector_blur_cc::linear {
using namespace blender::compositor;

#define MOTION_BLUR_TILE_SIZE 32
#define DEPTH_SCALE 100.0f

/* Returns the input velocity that has the larger magnitude. */
static float2 max_velocity(const float2 &a, const float2 &b)
{
  return math::length_squared(a) > math::length_squared(b) ? a : b;
}

/* Identical to motion_blur_tile_indirection_pack_payload, encodes the value and its texel such
 * that the integer length of the value is encoded in the most significant bits, then the x value
 * of the texel are encoded in the middle bits, then the y value of the texel is stored in the
 * least significant bits. */
static uint32_t velocity_atomic_max_value(const float2 &value, const int2 &texel)
{
  const uint32_t length_bits = math::min(uint32_t(math::ceil(math::length(value))), 0x3FFFu);
  return (length_bits << 18u) | ((texel.x & 0x1FFu) << 9u) | (texel.y & 0x1FFu);
}

/* Reduces each 32x32 block of velocity pixels into a single velocity whose magnitude is largest.
 * Each of the previous and next velocities are reduces independently. */
static Result compute_max_tile_velocity_cpu(Context &context, const Result &velocity_image)
{
  if (velocity_image.is_single_value()) {
    Result output = context.create_result(ResultType::Float4);
    output.allocate_single_value();
    output.set_single_value(velocity_image.get_single_value<float4>());
    return output;
  }

  const int2 tile_size = int2(MOTION_BLUR_TILE_SIZE);
  const int2 velocity_size = velocity_image.domain().data_size;
  const int2 tiles_count = math::divide_ceil(velocity_size, tile_size);
  Result output = context.create_result(ResultType::Float4);
  output.allocate_texture(Domain(tiles_count));

  parallel_for(tiles_count, [&](const int2 texel) {
    float2 max_previous_velocity = float2(0.0f);
    float2 max_next_velocity = float2(0.0f);

    for (int j = 0; j < tile_size.y; j++) {
      for (int i = 0; i < tile_size.x; i++) {
        int2 sub_texel = texel * tile_size + int2(i, j);
        const float4 velocity = velocity_image.load_pixel_extended<float4>(sub_texel);
        max_previous_velocity = max_velocity(velocity.xy(), max_previous_velocity);
        max_next_velocity = max_velocity(velocity.zw(), max_next_velocity);
      }
    }

    const float4 max_velocity = float4(max_previous_velocity, max_next_velocity);
    output.store_pixel(texel, max_velocity);
  });

  return output;
}

struct MotionRect {
  int2 bottom_left;
  int2 extent;
};

static MotionRect compute_motion_rect(const int2 &tile, const float2 &motion, const int2 &size)
{
  /* `ceil()` to number of tile touched. */
  int2 point1 = tile + int2(math::sign(motion) *
                            math::ceil(math::abs(motion) / float(MOTION_BLUR_TILE_SIZE)));
  int2 point2 = tile;

  /* Colour interpolation reads neighbouring guidance tiles. Include that support
   * in the conservative search, including for horizontal/vertical motion. */
  int2 max_point = math::max(point1, point2) + int2(1);
  int2 min_point = math::min(point1, point2) - int2(1);
  /* Clamp to bounds. */
  max_point = math::min(max_point, size - 1);
  min_point = math::max(min_point, int2(0));

  MotionRect rect;
  rect.bottom_left = min_point;
  rect.extent = 1 + max_point - min_point;
  return rect;
}

struct MotionLine {
  /** Origin of the line. */
  float2 origin;
  /** Normal to the line direction. */
  float2 normal;
};

static MotionLine compute_motion_line(const int2 &tile, const float2 &motion)
{
  float magnitude = math::length(motion);
  float2 dir = magnitude != 0.0f ? motion / magnitude : motion;

  MotionLine line;
  line.origin = float2(tile);
  /* Rotate 90 degrees counter-clockwise. */
  line.normal = float2(-dir.y, dir.x);
  return line;
}

static bool is_inside_motion_line(const int2 &tile, const MotionLine &motion_line)
{
  /* NOTE: Everything in is tile unit. */
  float distance_to_line = math::dot(motion_line.normal, motion_line.origin - float2(tile));
  /* In order to be conservative and for simplicity, we use the tiles bounding circles.
   * Consider that both the tile and the line have bounding radius of M_SQRT1_2. */
  return math::abs(distance_to_line) < std::numbers::sqrt2_v<float>;
}

/* The max tile velocity image computes the maximum within 32x32 blocks, while the velocity can
 * in fact extend beyond such a small block. So we dilate the max blocks by taking the maximum
 * along the path of each of the max velocity tiles. */
static Result dilate_max_velocity_cpu(Context &context,
                                      const Result &max_tile_velocity,
                                      const float shutter_speed)
{
  if (max_tile_velocity.is_single_value()) {
    Result output = context.create_result(ResultType::Float4);
    output.allocate_single_value();
    output.set_single_value(max_tile_velocity.get_single_value<float4>() *
                            float4(float2(shutter_speed), float2(-shutter_speed)));
    return output;
  }

  const int2 size = max_tile_velocity.domain().data_size;
  Result output = context.create_result(ResultType::Float4);
  output.allocate_texture(Domain(size));

  /* Preserve the winning source tile, matching the GPU's packed atomic maximum
   * when several motions have the same rounded magnitude. */
  Array<uint32_t> previous_payloads(int64_t(size.x) * size.y, 0u);
  Array<uint32_t> next_payloads(int64_t(size.x) * size.y, 0u);

  parallel_for(size, [&](const int2 texel) { output.store_pixel(texel, float4(0.0f)); });

  for (const int64_t y : IndexRange(size.y)) {
    for (const int64_t x : IndexRange(size.x)) {
      const int2 src_tile = int2(x, y);

      const float4 max_motion = max_tile_velocity.load_pixel<float4>(src_tile);
      const float2 max_previous_velocity = max_motion.xy() * shutter_speed;
      const float2 max_next_velocity = max_motion.zw() * -shutter_speed;

      {
        /* Rectangular area (in tiles) where the motion vector spreads. */
        MotionRect motion_rect = compute_motion_rect(src_tile, max_previous_velocity, size);
        MotionLine motion_line = compute_motion_line(src_tile, max_previous_velocity);
        /* Do a conservative rasterization of the line of the motion vector line. */
        for (int j = 0; j < motion_rect.extent.y; j++) {
          for (int i = 0; i < motion_rect.extent.x; i++) {
            int2 tile = motion_rect.bottom_left + int2(i, j);
            if (is_inside_motion_line(tile, motion_line)) {
              const float4 current_max_velocity = output.load_pixel<float4>(tile);
              const int64_t index = int64_t(tile.y) * size.x + tile.x;
              const uint32_t previous = velocity_atomic_max_value(max_previous_velocity, src_tile);
              const uint32_t next = velocity_atomic_max_value(max_next_velocity, src_tile);
              const float2 new_max_previous_velocity = previous > previous_payloads[index] ?
                                                           max_previous_velocity :
                                                           current_max_velocity.xy();
              const float2 new_max_next_velocity = next > next_payloads[index] ?
                                                       max_next_velocity :
                                                       current_max_velocity.zw();
              previous_payloads[index] = math::max(previous_payloads[index], previous);
              next_payloads[index] = math::max(next_payloads[index], next);
              output.store_pixel(tile, float4(new_max_previous_velocity, new_max_next_velocity));
            }
          }
        }
      }

      {
        /* Rectangular area (in tiles) where the motion vector spreads. */
        MotionRect motion_rect = compute_motion_rect(src_tile, max_next_velocity, size);
        MotionLine motion_line = compute_motion_line(src_tile, max_next_velocity);
        /* Do a conservative rasterization of the line of the motion vector line. */
        for (int j = 0; j < motion_rect.extent.y; j++) {
          for (int i = 0; i < motion_rect.extent.x; i++) {
            int2 tile = motion_rect.bottom_left + int2(i, j);
            if (is_inside_motion_line(tile, motion_line)) {
              const float4 current_max_velocity = output.load_pixel<float4>(tile);
              const int64_t index = int64_t(tile.y) * size.x + tile.x;
              const uint32_t previous = velocity_atomic_max_value(max_previous_velocity, src_tile);
              const uint32_t next = velocity_atomic_max_value(max_next_velocity, src_tile);
              const float2 new_max_previous_velocity = previous > previous_payloads[index] ?
                                                           max_previous_velocity :
                                                           current_max_velocity.xy();
              const float2 new_max_next_velocity = next > next_payloads[index] ?
                                                       max_next_velocity :
                                                       current_max_velocity.zw();
              previous_payloads[index] = math::max(previous_payloads[index], previous);
              next_payloads[index] = math::max(next_payloads[index], next);
              output.store_pixel(tile, float4(new_max_previous_velocity, new_max_next_velocity));
            }
          }
        }
      }
    }
  }

  return output;
}

/* Interleaved gradient noise by Jorge Jimenez
 * http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare. */
static float interleaved_gradient_noise(const int2 &p)
{
  return math::fract(52.9829189f * math::fract(0.06711056f * p.x + 0.00583715f * p.y));
}

static float2 spread_compare(const float center_motion_length,
                             const float sample_motion_length,
                             const float offset_length)
{
  return math::clamp(
      float2(center_motion_length, sample_motion_length) - offset_length + 1.0f, 0.0f, 1.0f);
}

static float2 depth_compare(const float center_depth, const float sample_depth)
{
  float2 depth_scale = float2(DEPTH_SCALE, -DEPTH_SCALE);
  return math::clamp(0.5f + depth_scale * (sample_depth - center_depth), 0.0f, 1.0f);
}

/* Kill contribution if not going the same direction. */
static float dir_compare(const float2 &offset,
                         const float2 &sample_motion,
                         const float &sample_motion_length)
{
  if (sample_motion_length < 0.5f) {
    return 1.0f;
  }
  return (math::dot(offset, sample_motion) > 0.0f) ? 1.0f : 0.0f;
}

/* Return background (x) and foreground (y) weights. */
static float2 sample_weights(const float center_depth,
                             const float sample_depth,
                             const float center_motion_length,
                             const float sample_motion_length,
                             const float offset_length)
{
  /* Classify foreground/background. */
  float2 depth_weight = depth_compare(center_depth, sample_depth);
  /* Weight if sample is overlapping or under the center pixel. */
  float2 spread_weight = spread_compare(center_motion_length, sample_motion_length, offset_length);
  return depth_weight * spread_weight;
}

struct Accumulator {
  float4 fg;
  float4 bg;
  /** x: Background, y: Foreground, z: dir. */
  float3 weight;
  float moving_foreground;
  /* Conditional directional colour, separate from the native exposure coverage.
   * Empty/background samples must not dilute the selected opacity envelope. */
  float4 streak;
  float streak_weight;
  float streak_faded_weight;
  /* Spatial footprint coverage is independent of shutter exposure/sample density. */
  float streak_coverage;
};

/* Values match the node's Falloff menu and the GPU exposure window. */
static float source_falloff(const int type, const float time)
{
  const float t = math::clamp(time, 0.0f, 1.0f);
  switch (type) {
    case 1:
      return 1.0f - t;
    case 2:
      return 1.0f - t * t * (3.0f - 2.0f * t);
    case 3:
      return 1.0f - t * t;
    case 4:
      return (1.0f - t) * (1.0f - t);
  }
  return 1.0f;
}

/* Average the window over the represented source interval. Clipping the interval
 * while keeping its jittered point's fade would underexpose short shutter paths. */
static float source_falloff_mean(const int type, const float start, const float end)
{
  const float a = math::clamp(start, 0.0f, 1.0f);
  const float b = math::clamp(end, 0.0f, 1.0f);
  const float mean_t = (a + b) * 0.5f;
  const float mean_t2 = (a * a + a * b + b * b) / 3.0f;
  const float mean_t3 = (a + b) * (a * a + b * b) * 0.25f;
  switch (type) {
    case 1:
      return 1.0f - mean_t;
    case 2:
      return math::clamp(1.0f - 3.0f * mean_t2 + 2.0f * mean_t3, 0.0f, 1.0f);
    case 3:
      return 1.0f - mean_t2;
    case 4:
      return math::clamp(1.0f - 2.0f * mean_t + mean_t2, 0.0f, 1.0f);
  }
  return 1.0f;
}

/* At a silhouette edge, interpolate only moving-source data. Averaging its vector
 * or depth with stationary/transparent pixels changes the source's shutter phase. */
static void sample_foreground_data(const Result &velocity,
                                   const Result &depth,
                                   const Result &coverage,
                                   const int2 &size,
                                   const float2 &uv,
                                   float4 &motion,
                                   float &sample_depth)
{
  const float2 position = uv * float2(size) - 0.5f;
  const int2 origin = int2(math::floor(position));
  const float2 factor = position - float2(origin);
  motion = float4(0.0f);
  sample_depth = 0.0f;
  float total_weight = 0.0f;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      const int2 texel = math::clamp(origin + int2(x, y), int2(0), size - 1);
      const float weight = (x ? factor.x : 1.0f - factor.x) * (y ? factor.y : 1.0f - factor.y) *
                           coverage.load_pixel<float, true>(texel);
      motion += velocity.load_pixel<float4, true>(texel) * weight;
      sample_depth += depth.load_pixel<float, true>(texel) * weight;
      total_weight += weight;
    }
  }
  if (total_weight > 1e-8f) {
    motion /= total_weight;
    sample_depth /= total_weight;
  }
}

static void gather_sample(const Result &input_image,
                          const Result &input_depth,
                          const Result &input_velocity,
                          const Result &foreground_image,
                          const Result &foreground_coverage,
                          const int2 &size,
                          const float2 &screen_uv,
                          const float center_depth,
                          const float center_motion_len,
                          const float2 &offset,
                          const float offset_len,
                          const bool next,
                          const float shutter_speed,
                          const int falloff,
                          const ShutterCurve &curve,
                          const float sample_weight,
                          const float integration_length,
                          const float sample_bin_start,
                          const float sample_step,
                          Accumulator &accum)
{
  float2 sample_uv = screen_uv - offset / float2(size);
  const float coverage = falloff != 0 ?
                             foreground_coverage.sample_bilinear_extended<float, true>(sample_uv) :
                             1.0f;
  if (coverage < 1e-8f) {
    return;
  }
  float4 sample_vectors = input_velocity.sample_bilinear_extended<float4, true>(sample_uv);
  float sample_depth = input_depth.sample_bilinear_extended<float, true>(sample_uv);
  if (falloff != 0 && coverage < 1.0f - 1e-6f) {
    sample_foreground_data(input_velocity,
                           input_depth,
                           foreground_coverage,
                           size,
                           sample_uv,
                           sample_vectors,
                           sample_depth);
  }
  sample_vectors *= float4(float2(shutter_speed), float2(-shutter_speed));
  float2 sample_motion = (next) ? sample_vectors.zw() : sample_vectors.xy();
  float sample_motion_len = math::length(sample_motion);
  const float4 sample_color = falloff == 0 ?
                                  float4(input_image.sample_bilinear_extended<Color>(sample_uv)) :
                                  float4(
                                      foreground_image.sample_bilinear_extended<Color>(sample_uv));

  float2 direct_weights = sample_weights(
      center_depth, sample_depth, center_motion_len, sample_motion_len, offset_len);

  float3 weights;
  weights.x = direct_weights.x;
  weights.y = direct_weights.y;
  weights.z = dir_compare(offset, sample_motion, sample_motion_len) * sample_weight;
  weights.x *= weights.z;
  weights.y *= weights.z;
  /* The proposal can be longer than this source's motion. Evaluate the window on
   * the source's own shutter phase, never the representative tile's sample time. */
  float fade = sample_motion_len >= 0.5f ?
                   source_falloff(falloff, offset_len / sample_motion_len) :
                   1.0f;
  if (falloff != 0) {
    /* Clip the sample's represented interval, not its jittered point. A hard
     * point test changes the denominator abruptly when filtered vectors round
     * across the shutter endpoint, imprinting one-sample opacity jumps. */
    const float interval_coverage =
        sample_motion_len >= 0.5f ?
            math::clamp((sample_motion_len - sample_bin_start) / sample_step, 0.0f, 1.0f) :
            1.0f;
    if (sample_motion_len >= 0.5f) {
      fade = falloff == 5 ?
                 curve.mean(next,
                            sample_bin_start / sample_motion_len,
                            (sample_bin_start + sample_step) / sample_motion_len) :
                 source_falloff_mean(falloff,
                                     sample_bin_start / sample_motion_len,
                                     (sample_bin_start + sample_step) / sample_motion_len);
    }
    const float foreground_weight = weights.y * interval_coverage;
    /* Bilinear coverage belongs to the sampling footprint, not intrinsic source
     * alpha. Normalize it out along with sample density, preserving translucent
     * sources and preventing opaque backgrounds from contaminating streak colour. */
    accum.streak += sample_color * (foreground_weight * fade);
    accum.streak_weight += foreground_weight * coverage;
    accum.streak_faded_weight += foreground_weight * coverage * fade;
    if (foreground_weight > 0.0f) {
      /* A bin barely intersecting the source interval must not acquire a full
       * footprint when conditional normalization cancels its tiny weight. */
      accum.streak_coverage = math::max(accum.streak_coverage, coverage * interval_coverage);
    }
  }
  else if (integration_length > 0.0f) {
    /* The native depth test splits equal-depth samples between foreground and
     * background. With an independently reconstructed background, moving-source
     * coverage gets the full equal-depth weight, while farther samples can fade. */
    const float4 foreground_color = float4(
        foreground_image.sample_bilinear_extended<Color>(sample_uv));
    /* The one-pixel spread ramp adds half a pixel of support to each shutter
     * half. Normalize exposure over the source's own support, not tile length. */
    const float kernel_weight = integration_length / (sample_motion_len + 0.5f);
    accum.streak += foreground_color *
                    (math::min(2.0f * direct_weights.y, 1.0f) * weights.z * kernel_weight);
  }
  weights.x *= fade;
  weights.y *= fade;
  if (sample_motion_len >= 0.5f && sample_color.w > 0.0f && sample_depth > 0.0f &&
      center_depth > 0.0f && sample_depth < center_depth - 0.01f)
  {
    accum.moving_foreground += weights.y;
  }

  accum.fg += sample_color * weights.y;
  accum.bg += sample_color * weights.x;
  accum.weight += weights;
}

/* Balance overlapping recovery/guidance proposals without counting their shared
 * interval twice. Proposal probability ends at its actual shutter segment; the
 * source's spread ramp is a separate spatial reconstruction kernel. */
static float integration_sample_weight(const float ratio,
                                       const float distance,
                                       const float other_length,
                                       const float cosine,
                                       const float sine)
{
  if (other_length < 0.5f) {
    return ratio;
  }
  const float projected = distance * cosine;
  const float overlap = projected >= 0.0f && projected <= other_length ?
                            math::clamp(1.0f - distance * sine, 0.0f, 1.0f) :
                            0.0f;
  return ratio * (2.0f / (1.0f + overlap));
}

static void gather_blur(const Result &input_image,
                        const Result &input_depth,
                        const Result &input_velocity,
                        const Result &foreground_image,
                        const Result &foreground_coverage,
                        const int2 &size,
                        const float2 &screen_uv,
                        const float2 &center_motion,
                        const float center_depth,
                        const float2 &max_motion,
                        const float ofs,
                        const bool next,
                        const int samples_count,
                        const float shutter_speed,
                        const int falloff,
                        const ShutterCurve &curve,
                        const bool use_integration_weights,
                        Accumulator &accum)
{
  float center_motion_len = math::length(center_motion);
  float max_motion_len = math::length(max_motion);
  const float integration_length = math::max(center_motion_len, max_motion_len);
  float cosine = 1.0f;
  float sine = 0.0f;
  if (use_integration_weights && center_motion != max_motion && center_motion_len >= 0.5f &&
      max_motion_len >= 0.5f)
  {
    cosine = math::clamp(
        math::dot(center_motion, max_motion) / (center_motion_len * max_motion_len), -1.0f, 1.0f);
    sine = math::sqrt(math::max(0.0f, 1.0f - cosine * cosine));
  }

  /* Equal sample counts represent different spatial densities on short and long
   * proposals. Integrate by covered distance, preserving the input velocities. */

  int i;
  float t, inc = 1.0f / float(samples_count);
  if (max_motion_len >= 0.5f) {
    for (i = 0, t = ofs * inc; i < samples_count; i++, t += inc) {
      gather_sample(input_image,
                    input_depth,
                    input_velocity,
                    foreground_image,
                    foreground_coverage,
                    size,
                    screen_uv,
                    center_depth,
                    center_motion_len,
                    max_motion * t,
                    max_motion_len * t,
                    next,
                    shutter_speed,
                    falloff,
                    curve,
                    use_integration_weights ?
                        integration_sample_weight(max_motion_len / integration_length,
                                                  max_motion_len * t,
                                                  center_motion_len,
                                                  cosine,
                                                  sine) :
                        1.0f,
                    use_integration_weights ? integration_length : 0.0f,
                    float(i) * max_motion_len * inc,
                    max_motion_len * inc,
                    accum);
    }
  }

  if (center_motion_len < 0.5f) {
    return;
  }

  for (i = 0, t = ofs * inc; i < samples_count; i++, t += inc) {
    /* Also sample in center motion direction.
     * Allow recovering motion where there is conflicting
     * motion between foreground and background. */
    gather_sample(input_image,
                  input_depth,
                  input_velocity,
                  foreground_image,
                  foreground_coverage,
                  size,
                  screen_uv,
                  center_depth,
                  center_motion_len,
                  center_motion * t,
                  center_motion_len * t,
                  next,
                  shutter_speed,
                  falloff,
                  curve,
                  use_integration_weights ?
                      integration_sample_weight(center_motion_len / integration_length,
                                                center_motion_len * t,
                                                max_motion_len,
                                                cosine,
                                                sine) :
                      1.0f,
                  use_integration_weights ? integration_length : 0.0f,
                  float(i) * center_motion_len * inc,
                  center_motion_len * inc,
                  accum);
  }
}

/* Resolve each proposal before interpolation. Combining its normalized weights
 * first would cancel small tile weights and recreate a hard boundary at zero. */
static float4 reconstruct_pixel(const Result &image,
                                const Result &depth,
                                const Result &velocity,
                                const Result &background,
                                const Result &foreground_image,
                                const Result &foreground_coverage,
                                const int2 &size,
                                const int2 &texel,
                                const float2 &uv,
                                const float4 &center_motion,
                                const float4 &max_motion,
                                const float center_depth,
                                float4 center_color,
                                const float random,
                                const int samples_count,
                                const float shutter_speed,
                                const int falloff,
                                const ShutterCurve &curve)
{
  if (math::max(math::max(math::length(center_motion.xy()), math::length(center_motion.zw())),
                math::max(math::length(max_motion.xy()), math::length(max_motion.zw()))) < 0.5f)
  {
    return center_color;
  }
  Accumulator accum;
  const int2 seed = background.load_pixel<int2, true>(texel);
  accum.weight = float3(0.0f, 0.0f, 1.0f);
  accum.bg = float4(0.0f);
  accum.fg = float4(0.0f);
  accum.moving_foreground = 0.0f;
  accum.streak = float4(0.0f);
  accum.streak_weight = 0.0f;
  accum.streak_faded_weight = 0.0f;
  accum.streak_coverage = falloff != 0 ? foreground_coverage.load_pixel<float, true>(texel) : 0.0f;
  for (const bool next : {false, true}) {
    gather_blur(image,
                depth,
                velocity,
                foreground_image,
                foreground_coverage,
                size,
                uv,
                next ? center_motion.zw() : center_motion.xy(),
                center_depth,
                next ? max_motion.zw() : max_motion.xy(),
                random,
                next,
                samples_count,
                shutter_speed,
                falloff,
                curve,
                seed != JUMP_FLOODING_NON_FLOODED_VALUE,
                accum);
  }
  if (center_depth > 0.0f && center_color.w >= 1.0f &&
      math::max(math::length(center_motion.xy()), math::length(center_motion.zw())) < 0.5f &&
      accum.moving_foreground == 0.0f)
  {
    return center_color;
  }
  if (falloff != 0) {
    if (seed == JUMP_FLOODING_NON_FLOODED_VALUE) {
      return accum.streak_faded_weight > 1e-8f ? accum.streak / accum.streak_faded_weight :
                                                 (falloff == 5 ? float4(0.0f) : center_color);
    }
    const float4 bg = float4(image.load_pixel<Color>(seed));
    const float4 streak = accum.streak_weight > 1e-8f ?
                              accum.streak * (accum.streak_coverage / accum.streak_weight) :
                              float4(0.0f);
    return streak + bg * (1.0f - math::clamp(streak.w, 0.0f, 1.0f));
  }
  if (seed != JUMP_FLOODING_NON_FLOODED_VALUE) {
    /* A short, coverage-mixed edge vector must not make the current moving
     * silhouette a background colour sample. Resolve uniform exposure over the
     * same stationary estimate used by selected falloffs. */
    const float4 bg = float4(image.load_pixel<Color>(seed));
    float4 streak = accum.streak / accum.weight.z;
    if (streak.w > 1.0f) {
      streak /= streak.w;
    }
    return streak + bg * (1.0f - math::clamp(streak.w, 0.0f, 1.0f));
  }
  /* Native foreground/background reconstruction, including its tiny fallback. */
  const float epsilon = 1.0f / (50.0f * float(samples_count) * 4.0f);
  accum.bg += center_color * epsilon;
  accum.weight.x += epsilon;
  center_color = accum.bg / accum.weight.x;
  accum.fg += accum.bg;
  accum.weight.y += accum.weight.x;
  const float blend = math::clamp(1.0f - accum.weight.y / accum.weight.z, 0.0f, 1.0f);
  return accum.fg / accum.weight.z + center_color * blend;
}

static void motion_blur_cpu(const Result &input_image,
                            const Result &input_depth,
                            const Result &input_velocity,
                            const Result &max_velocity,
                            const Result &background,
                            const Result &foreground_image,
                            const Result &foreground_coverage,
                            Result &output,
                            const int samples_count,
                            const float shutter_speed,
                            const int falloff,
                            const ShutterCurve &curve)
{
  const int2 size = input_image.domain().data_size;
  threading::parallel_for(IndexRange(size.y), 1, [&](const IndexRange sub_y_range) {
    for (const int64_t y : sub_y_range) {
      for (const int64_t x : IndexRange(size.x)) {
        const int2 texel(x, y);
        const float2 uv = (float2(texel) + 0.5f) / float2(size);
        const float center_depth = input_depth.load_pixel<float, true>(texel);
        const float4 center_motion = input_velocity.load_pixel<float4, true>(texel) *
                                     float4(float2(shutter_speed), float2(-shutter_speed));
        const float4 center_color = float4(input_image.load_pixel<Color>(texel));
        const float random = interleaved_gradient_noise(texel);
        float4 guides[4];
        float weights[4];
        int guides_count = 0;
        if (max_velocity.is_single_value()) {
          guides[0] = max_velocity.get_single_value<float4>();
          weights[0] = 1.0f;
          guides_count = 1;
        }
        else {
          const float2 position = (float2(texel) + 0.5f) / float(MOTION_BLUR_TILE_SIZE) - 0.5f;
          const int2 origin = int2(math::floor(position));
          const float2 factor = position - float2(origin);
          for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
              const float weight = (i ? factor.x : 1.0f - factor.x) *
                                   (j ? factor.y : 1.0f - factor.y);
              if (weight == 0.0f) {
                continue;
              }
              const float4 guide = max_velocity.load_pixel_extended<float4>(origin + int2(i, j));
              int index = 0;
              for (; index < guides_count; index++) {
                if (guides[index] == guide) {
                  break;
                }
              }
              if (index == guides_count) {
                guides[guides_count] = guide;
                weights[guides_count++] = 0.0f;
              }
              weights[index] += weight;
            }
          }
        }
        float4 color(0.0f);
        for (int i = 0; i < guides_count; i++) {
          color += reconstruct_pixel(input_image,
                                     input_depth,
                                     input_velocity,
                                     background,
                                     foreground_image,
                                     foreground_coverage,
                                     size,
                                     texel,
                                     uv,
                                     center_motion,
                                     guides[i],
                                     center_depth,
                                     center_color,
                                     random,
                                     samples_count,
                                     shutter_speed,
                                     falloff,
                                     curve) *
                   weights[i];
        }
        output.store_pixel(texel, Color(color));
      }
    }
  });
}

static void blur_gpu(Context &context,
                     const Result &image,
                     const Result &depth,
                     const Result &velocity,
                     const Result &background,
                     const Result &foreground_image,
                     const Result &foreground_coverage,
                     Result &output,
                     const Domain &domain,
                     const int samples,
                     const float shutter,
                     const int falloff,
                     const ShutterCurve &curve)
{
  Result tiles = context.create_result(ResultType::Float4);
  gpu::StorageBuf *indirection = nullptr;
  if (velocity.is_single_value()) {
    tiles.allocate_single_value();
    tiles.set_single_value(velocity.get_single_value<float4>());
    indirection = GPU_storagebuf_create_ex(
        sizeof(uint32_t), nullptr, GPU_USAGE_DEVICE_ONLY, __func__);
  }
  else {
    const int2 count = math::divide_ceil(velocity.domain().data_size, int2(32));
    tiles.allocate_texture(Domain(count));
    gpu::Shader *reduce = context.get_shader("compositor_motion_blur_linear_tiles");
    GPU_shader_bind(reduce);
    velocity.bind_as_texture(reduce, "input_tx");
    tiles.bind_as_image(reduce, "output_img");
    GPU_compute_dispatch(reduce, count.x, count.y, 1);
    GPU_shader_unbind();
    velocity.unbind_as_texture();
    tiles.unbind_as_image();

    indirection = GPU_storagebuf_create_ex(
        sizeof(uint32_t) * 512 * 512 * 2, nullptr, GPU_USAGE_DEVICE_ONLY, __func__);
    GPU_storagebuf_clear_to_zero(indirection);
    gpu::Shader *dilate = context.get_shader("compositor_motion_blur_max_velocity_dilate");
    GPU_shader_bind(dilate);
    GPU_shader_uniform_1f(dilate, "shutter_speed", shutter);
    tiles.bind_as_texture(dilate, "input_tx");
    GPU_storagebuf_bind(indirection, GPU_shader_get_ssbo_binding(dilate, "tile_indirection_buf"));
    compute_dispatch_threads_at_least(dilate, count);
    GPU_shader_unbind();
    tiles.unbind_as_texture();
  }

  gpu::Shader *shader = context.get_shader("compositor_motion_blur_linear");
  GPU_shader_bind(shader);
  GPU_shader_uniform_1i(shader, "samples_count", samples);
  GPU_shader_uniform_1f(shader, "shutter_speed", shutter);
  GPU_shader_uniform_1i(shader, "falloff_type", falloff);
  const Span<float2> curve_table = curve.table();
  gpu::StorageBuf *curve_buffer = GPU_storagebuf_create_ex(
      curve_table.size_in_bytes(), curve_table.data(), GPU_USAGE_STATIC, __func__);
  GPU_storagebuf_bind(curve_buffer, GPU_shader_get_ssbo_binding(shader, "shutter_curve_buf"));
  GPU_shader_uniform_1b(shader, "single_velocity", velocity.is_single_value());
  image.bind_as_texture(shader, "input_tx");
  gpu::Texture *depth_texture = depth.bind_as_texture_or_single_value(shader, "depth_tx");
  gpu::Texture *velocity_texture = velocity.bind_as_texture_or_single_value(shader, "velocity_tx");
  gpu::Texture *tiles_texture = tiles.bind_as_texture_or_single_value(shader, "max_velocity_tx");
  gpu::Texture *background_texture = background.bind_as_texture_or_single_value(shader,
                                                                                "background_tx");
  gpu::Texture *foreground_texture = foreground_image.bind_as_texture_or_single_value(
      shader, "foreground_tx");
  gpu::Texture *coverage_texture = foreground_coverage.bind_as_texture_or_single_value(
      shader, "coverage_tx");
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_storagebuf_bind(indirection, GPU_shader_get_ssbo_binding(shader, "tile_indirection_buf"));
  output.allocate_texture(domain);
  output.bind_as_image(shader, "output_img");
  compute_dispatch_threads_at_least(shader, domain.data_size);
  GPU_shader_unbind();
  image.unbind_as_texture();
  depth.unbind_as_texture_or_single_value(depth_texture);
  velocity.unbind_as_texture_or_single_value(velocity_texture);
  tiles.unbind_as_texture_or_single_value(tiles_texture);
  background.unbind_as_texture_or_single_value(background_texture);
  foreground_image.unbind_as_texture_or_single_value(foreground_texture);
  foreground_coverage.unbind_as_texture_or_single_value(coverage_texture);
  output.unbind_as_image();
  tiles.release();
  GPU_storagebuf_free(indirection);
  GPU_storagebuf_free(curve_buffer);
}

void blur(Context &context,
          const Result &image,
          const Result &depth,
          const Result &velocity,
          Result &output,
          const Domain &domain,
          const int samples,
          const float shutter,
          const int falloff,
          const ShutterCurve &curve)
{
  Result background = context.create_result(ResultType::Int2, ResultPrecision::Half);
  Result foreground_image = context.create_result(ResultType::Color);
  Result foreground_coverage = context.create_result(ResultType::Float);
  if (velocity.meta_data.is_estimated_motion) {
    /* Footage only tells us the visible colour. Do not flood from pixels whose flow was
     * rejected, or assume a clean plate exists behind their neighbours. */
    background.allocate_single_value();
    background.set_single_value(JUMP_FLOODING_NON_FLOODED_VALUE);
    foreground_image.share_data(image);
    foreground_coverage.allocate_single_value();
    foreground_coverage.set_single_value(1.0f);
  }
  else {
    /* Reconstruct the background under moving pixels without adding a sharp copy
     * of their current silhouette. This is the same estimate used by curved paths. */
    Result seeds = context.create_result(ResultType::Int2, ResultPrecision::Half);
    seeds.allocate_texture(domain);
    foreground_image.allocate_texture(domain);
    foreground_coverage.allocate_texture(domain);
    if (context.use_gpu()) {
      gpu::Shader *shader = context.get_shader("compositor_motion_blur_linear_background");
      GPU_shader_bind(shader);
      GPU_shader_uniform_1f(shader, "shutter_speed", shutter);
      image.bind_as_texture(shader, "input_tx");
      gpu::Texture *velocity_texture = velocity.bind_as_texture_or_single_value(shader,
                                                                                "velocity_tx");
      seeds.bind_as_image(shader, "seeds_img");
      foreground_image.bind_as_image(shader, "foreground_img");
      foreground_coverage.bind_as_image(shader, "coverage_img");
      compute_dispatch_threads_at_least(shader, domain.data_size);
      GPU_shader_unbind();
      image.unbind_as_texture();
      velocity.unbind_as_texture_or_single_value(velocity_texture);
      seeds.unbind_as_image();
      foreground_image.unbind_as_image();
      foreground_coverage.unbind_as_image();
    }
    else {
      parallel_for(domain.data_size, [&](const int2 texel) {
        const float4 color = float4(image.load_pixel<Color>(texel));
        const float4 motion = velocity.load_pixel<float4, true>(texel) * shutter;
        const bool moving = color != float4(0.0f) && math::max(math::length(motion.xy()),
                                                               math::length(motion.zw())) >= 1e-5f;
        seeds.store_pixel(texel, initialize_jump_flooding_value(texel, !moving));
        foreground_image.store_pixel(texel, Color(moving ? color : float4(0.0f)));
        foreground_coverage.store_pixel(texel, moving ? 1.0f : 0.0f);
      });
    }
    jump_flooding(context, seeds, background);
    seeds.release();
  }
  if (context.use_gpu()) {
    blur_gpu(context,
             image,
             depth,
             velocity,
             background,
             foreground_image,
             foreground_coverage,
             output,
             domain,
             samples,
             shutter,
             falloff,
             curve);
    background.release();
    foreground_image.release();
    foreground_coverage.release();
    return;
  }
  output.allocate_texture(domain);
  Result tiles = compute_max_tile_velocity_cpu(context, velocity);
  Result dilated = dilate_max_velocity_cpu(context, tiles, shutter);
  tiles.release();
  motion_blur_cpu(image,
                  depth,
                  velocity,
                  dilated,
                  background,
                  foreground_image,
                  foreground_coverage,
                  output,
                  samples,
                  shutter,
                  falloff,
                  curve);
  dilated.release();
  background.release();
  foreground_image.release();
  foreground_coverage.release();
}

}  // namespace blender::nodes::node_composite_vector_blur_cc::linear
