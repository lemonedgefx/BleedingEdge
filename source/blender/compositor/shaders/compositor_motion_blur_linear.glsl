/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* This is identical to the EEVEE implementation in eevee_motion_blur_gather_comp.glsl with the
 * necessary adjustments to make it work for the compositor:
 *
 *   - depth_compare() uses an inverted sign since the depth texture stores linear depth.
 *   - The next velocities are inverted since the velocity textures stores the previous and next
 *     velocities in the same direction.
 *   - The samples count is a variable uniform and not fixed to 8 samples.
 *   - The depth scale is constant and set to 100.
 *   - The motion scale is defined by the shutter_speed. */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur_linear)

#include "gpu_shader_compositor_motion_blur_lib.glsl"
#include "gpu_shader_compositor_motion_blur_linear_lib.glsl"
#include "gpu_shader_compositor_motion_blur_shutter_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

#define g_depth_scale 100.0f

/* Interleaved gradient noise by Jorge Jimenez
 * http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare. */
float interleaved_gradient_noise(int2 p)
{
  return fract(52.9829189f * fract(0.06711056f * p.x + 0.00583715f * p.y));
}

float2 spread_compare(float center_motion_length, float sample_motion_length, float offset_length)
{
  return clamp(
      float2(center_motion_length, sample_motion_length) - offset_length + 1.0f, 0.0f, 1.0f);
}

float2 depth_compare(float center_depth, float sample_depth)
{
  float2 depth_scale = float2(g_depth_scale, -g_depth_scale);
  return clamp(0.5f + depth_scale * (sample_depth - center_depth), 0.0f, 1.0f);
}

/* Kill contribution if not going the same direction. */
float dir_compare(float2 offset, float2 sample_motion, float sample_motion_length)
{
  if (sample_motion_length < 0.5f) {
    return 1.0f;
  }
  return (dot(offset, sample_motion) > 0.0f) ? 1.0f : 0.0f;
}

/* Return background (x) and foreground (y) weights. */
float2 sample_weights(float center_depth,
                      float sample_depth,
                      float center_motion_length,
                      float sample_motion_length,
                      float offset_length)
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
  float4 streak;
  float streak_weight;
  float streak_faded_weight;
  float streak_coverage;
};

/* Match the CPU window integral, including a partially covered final bin. */
float source_falloff_mean(const int type, const float start, const float end)
{
  float a = clamp(start, 0.0f, 1.0f);
  float b = clamp(end, 0.0f, 1.0f);
  float mean_t = (a + b) * 0.5f;
  float mean_t2 = (a * a + a * b + b * b) / 3.0f;
  float mean_t3 = (a + b) * (a * a + b * b) * 0.25f;
  switch (type) {
    case 1:
      return 1.0f - mean_t;
    case 2:
      return clamp(1.0f - 3.0f * mean_t2 + 2.0f * mean_t3, 0.0f, 1.0f);
    case 3:
      return 1.0f - mean_t2;
    case 4:
      return clamp(1.0f - 2.0f * mean_t + mean_t2, 0.0f, 1.0f);
  }
  return 1.0f;
}

void sample_foreground_data(float2 uv, float4 &motion, float &sample_depth)
{
  int2 size = texture_size(input_tx);
  float2 position = uv * float2(size) - 0.5f;
  int2 origin = int2(floor(position));
  float2 factor = position - float2(origin);
  motion = float4(0.0f);
  sample_depth = 0.0f;
  float total_weight = 0.0f;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      int2 texel = clamp(origin + int2(x, y), int2(0), size - 1);
      float weight = (x != 0 ? factor.x : 1.0f - factor.x) *
                     (y != 0 ? factor.y : 1.0f - factor.y) * texture_load(coverage_tx, texel).r;
      motion += texture_load(velocity_tx, texel) * weight;
      sample_depth += texture_load(depth_tx, texel).r * weight;
      total_weight += weight;
    }
  }
  if (total_weight > 1e-8f) {
    motion /= total_weight;
    sample_depth /= total_weight;
  }
}

void gather_sample(float2 screen_uv,
                   float center_depth,
                   float center_motion_len,
                   float2 offset,
                   float offset_len,
                   const bool next,
                   const float sample_weight,
                   const float integration_length,
                   const float sample_bin_start,
                   const float sample_step,
                   Accumulator &accum)
{
  float2 sample_uv = screen_uv - offset / float2(texture_size(input_tx));
  float coverage = falloff_type != 0 ? texture(coverage_tx, sample_uv).r : 1.0f;
  if (coverage < 1e-8f) {
    return;
  }
  float4 sample_vectors = texture(velocity_tx, sample_uv);
  float sample_depth = texture(depth_tx, sample_uv).r;
  if (falloff_type != 0 && coverage < 1.0f - 1e-6f) {
    sample_foreground_data(sample_uv, sample_vectors, sample_depth);
  }
  sample_vectors *= float4(float2(shutter_speed), float2(-shutter_speed));
  float2 sample_motion = (next) ? sample_vectors.zw : sample_vectors.xy;
  float sample_motion_len = length(sample_motion);
  float4 sample_color = falloff_type == 0 ? texture(input_tx, sample_uv) :
                                            texture(foreground_tx, sample_uv);

  float2 direct_weights = sample_weights(
      center_depth, sample_depth, center_motion_len, sample_motion_len, offset_len);
  float3 weights;
  weights.xy = direct_weights;
  weights.z = dir_compare(offset, sample_motion, sample_motion_len) * sample_weight;
  weights.xy *= weights.z;
  /* Source-relative phase makes short and long streaks fade over their own extent. */
  float fade = sample_motion_len >= 0.5f ?
                   motion_blur_falloff(falloff_type, offset_len / sample_motion_len) :
                   1.0f;
  if (falloff_type != 0) {
    float interval_coverage = sample_motion_len >= 0.5f ?
                                  clamp((sample_motion_len - sample_bin_start) / sample_step,
                                        0.0f,
                                        1.0f) :
                                  1.0f;
    if (sample_motion_len >= 0.5f) {
      fade = falloff_type == 5 ?
                 shutter_curve_mean(next,
                                    sample_bin_start / sample_motion_len,
                                    (sample_bin_start + sample_step) / sample_motion_len) :
                 source_falloff_mean(falloff_type,
                                     sample_bin_start / sample_motion_len,
                                     (sample_bin_start + sample_step) / sample_motion_len);
    }
    float foreground_weight = weights.y * interval_coverage;
    /* Remove sampling footprint coverage, keeping intrinsic source alpha. */
    accum.streak += sample_color * (foreground_weight * fade);
    accum.streak_weight += foreground_weight * coverage;
    accum.streak_faded_weight += foreground_weight * coverage * fade;
    if (foreground_weight > 0.0f) {
      accum.streak_coverage = max(accum.streak_coverage, coverage * interval_coverage);
    }
  }
  else if (integration_length > 0.0f) {
    float4 foreground_color = texture(foreground_tx, sample_uv);
    float foreground_weight = min(2.0f * direct_weights.y, 1.0f);
    float kernel_weight = integration_length / (sample_motion_len + 0.5f);
    accum.streak += foreground_color * (foreground_weight * weights.z * kernel_weight);
  }
  weights.xy *= fade;
  if (sample_motion_len >= 0.5f && sample_color.w > 0.0f && sample_depth > 0.0f &&
      center_depth > 0.0f && sample_depth < center_depth - 0.01f)
  {
    accum.moving_foreground += weights.y;
  }

  accum.fg += sample_color * weights.y;
  accum.bg += sample_color * weights.x;
  accum.weight += weights;
}

float integration_sample_weight(
    float ratio, float distance, float other_length, float cosine, float sine)
{
  if (other_length < 0.5f) {
    return ratio;
  }
  float projected = distance * cosine;
  float overlap = projected >= 0.0f && projected <= other_length ?
                      clamp(1.0f - distance * sine, 0.0f, 1.0f) :
                      0.0f;
  return ratio * (2.0f / (1.0f + overlap));
}

void gather_blur(float2 screen_uv,
                 float2 center_motion,
                 float center_depth,
                 float2 max_motion,
                 float ofs,
                 const bool next,
                 const bool use_integration_weights,
                 Accumulator &accum)
{
  float center_motion_len = length(center_motion);
  float max_motion_len = length(max_motion);
  float integration_length = max(center_motion_len, max_motion_len);
  float cosine = 1.0f;
  float sine = 0.0f;
  if (use_integration_weights && any(notEqual(center_motion, max_motion)) &&
      center_motion_len >= 0.5f && max_motion_len >= 0.5f)
  {
    cosine = clamp(
        dot(center_motion, max_motion) / (center_motion_len * max_motion_len), -1.0f, 1.0f);
    sine = sqrt(max(0.0f, 1.0f - cosine * cosine));
  }

  /* Equal sample counts represent different spatial densities on short and long
   * proposals. Integrate by covered distance, preserving the input velocities. */

  int i;
  float t, inc = 1.0f / float(samples_count);
  if (max_motion_len >= 0.5f) {
    for (i = 0, t = ofs * inc; i < samples_count; i++, t += inc) {
      gather_sample(screen_uv,
                    center_depth,
                    center_motion_len,
                    max_motion * t,
                    max_motion_len * t,
                    next,
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
    gather_sample(screen_uv,
                  center_depth,
                  center_motion_len,
                  center_motion * t,
                  center_motion_len * t,
                  next,
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

float4 reconstruct_pixel(int2 texel,
                         float2 uv,
                         float4 center_motion,
                         float4 max_motion,
                         float center_depth,
                         float4 center_color,
                         float random)
{
  if (max(max(length(center_motion.xy), length(center_motion.zw)),
          max(length(max_motion.xy), length(max_motion.zw))) < 0.5f)
  {
    return center_color;
  }
  Accumulator accum;
  int2 seed = texture_load(background_tx, texel).xy;
  bool use_integration_weights = any(notEqual(seed, int2(-1)));
  accum.weight = float3(0.0f, 0.0f, 1.0f);
  accum.bg = float4(0.0f);
  accum.fg = float4(0.0f);
  accum.moving_foreground = 0.0f;
  accum.streak = float4(0.0f);
  accum.streak_weight = 0.0f;
  accum.streak_faded_weight = 0.0f;
  accum.streak_coverage = falloff_type != 0 ? texture_load(coverage_tx, texel).r : 0.0f;
  gather_blur(uv,
              center_motion.xy,
              center_depth,
              max_motion.xy,
              random,
              false,
              use_integration_weights,
              accum);
  gather_blur(uv,
              center_motion.zw,
              center_depth,
              max_motion.zw,
              random,
              true,
              use_integration_weights,
              accum);
  if (center_depth > 0.0f && center_color.w >= 1.0f &&
      max(length(center_motion.xy), length(center_motion.zw)) < 0.5f &&
      accum.moving_foreground == 0.0f)
  {
    return center_color;
  }
  if (falloff_type != 0) {
    if (all(equal(seed, int2(-1)))) {
      return accum.streak_faded_weight > 1e-8f ? accum.streak / accum.streak_faded_weight :
                                                 (falloff_type == 5 ? float4(0.0f) : center_color);
    }
    float4 bg = texture_load(input_tx, seed);
    float4 streak = accum.streak_weight > 1e-8f ?
                        accum.streak * (accum.streak_coverage / accum.streak_weight) :
                        float4(0.0f);
    return streak + bg * (1.0f - clamp(streak.w, 0.0f, 1.0f));
  }
  if (any(notEqual(seed, int2(-1)))) {
    float4 bg = texture_load(input_tx, seed);
    float4 streak = accum.streak / accum.weight.z;
    if (streak.w > 1.0f) {
      streak /= streak.w;
    }
    return streak + bg * (1.0f - clamp(streak.w, 0.0f, 1.0f));
  }
  float epsilon = 1.0f / (50.0f * float(samples_count) * 4.0f);
  accum.bg += center_color * epsilon;
  accum.weight.x += epsilon;
  center_color = accum.bg / accum.weight.x;
  accum.fg += accum.bg;
  accum.weight.y += accum.weight.x;
  float blend = clamp(1.0f - accum.weight.y / accum.weight.z, 0.0f, 1.0f);
  return accum.fg / accum.weight.z + center_color * blend;
}

float4 load_guidance(int2 tile)
{
  tile = clamp(tile, int2(0), texture_size(max_velocity_tx) - 1);
  uint previous =
      tile_indirection_buf[motion_blur_tile_indirection_index(MOTION_PREV, uint2(tile))];
  uint next = tile_indirection_buf[motion_blur_tile_indirection_index(MOTION_NEXT, uint2(tile))];
  float4 motion;
  motion.xy =
      previous == 0u ?
          float2(0.0f) :
          texture_load(max_velocity_tx, motion_blur_tile_indirection_pack_payload(previous)).xy;
  motion.zw =
      next == 0u ?
          float2(0.0f) :
          texture_load(max_velocity_tx, motion_blur_tile_indirection_pack_payload(next)).zw;
  return motion * float4(float2(shutter_speed), float2(-shutter_speed));
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, texture_size(input_tx)))) {
    return;
  }
  float2 uv = (float2(texel) + 0.5f) / float2(texture_size(input_tx));
  float center_depth = texture_load(depth_tx, texel).x;
  float4 center_motion = texture(velocity_tx, uv) *
                         float4(float2(shutter_speed), float2(-shutter_speed));
  float4 center_color = textureLod(input_tx, uv, 0.0f);
  float random = interleaved_gradient_noise(texel);
  float4 guides[4];
  float weights[4];
  int guides_count = 0;
  if (single_velocity) {
    guides[0] = center_motion;
    weights[0] = 1.0f;
    guides_count = 1;
  }
  else {
    /* Interpolate reconstructed colours, never opposing velocity vectors. */
    float2 position = (float2(texel) + 0.5f) / float(MOTION_BLUR_TILE_SIZE) - 0.5f;
    int2 origin = int2(floor(position));
    float2 factor = position - float2(origin);
    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        float weight = (i != 0 ? factor.x : 1.0f - factor.x) *
                       (j != 0 ? factor.y : 1.0f - factor.y);
        if (weight == 0.0f) {
          continue;
        }
        float4 guide = load_guidance(origin + int2(i, j));
        int index = 0;
        for (; index < guides_count; index++) {
          if (all(equal(guides[index], guide))) {
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
  float4 color = float4(0.0f);
  for (int i = 0; i < guides_count; i++) {
    color += reconstruct_pixel(
                 texel, uv, center_motion, guides[i], center_depth, center_color, random) *
             weights[i];
  }
  imageStore(output_img, texel, color);
}
