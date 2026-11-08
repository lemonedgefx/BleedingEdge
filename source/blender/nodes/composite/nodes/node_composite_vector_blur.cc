/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>
#include <cmath>
#include <cstdint>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.hh"

#include "BKE_colortools.hh"

#include "BLO_read_write.hh"

#include "DNA_color_types.h"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "RNA_types.hh"

#include "COM_algorithm_jump_flooding.hh"
#include "COM_node_operation.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "node_composite_vector_blur_linear.hh"
#include "node_composite_vector_blur_shutter.hh"

namespace blender::nodes::node_composite_vector_blur_cc {

/* Keep these values in sync with motion_blur_falloff in the GPU shader. */
enum class FalloffType { None = 0, Linear = 1, Smooth = 2, EaseIn = 3, EaseOut = 4, Custom = 5 };

static const EnumPropertyItem falloff_items[] = {
    {0, "NONE", 0, N_("None"), N_("Uniform exposure weighting without an endpoint fade")},
    {1, "LINEAR", 0, N_("Linear"), N_("Fade trails linearly toward each shutter endpoint")},
    {2, "SMOOTH", 0, N_("Smooth"), N_("Fade trails with a smooth transition at both ends")},
    {3,
     "EASE_IN",
     0,
     N_("Ease In"),
     N_("Keep trails stronger near the original, then fade faster")},
    {4, "EASE_OUT", 0, N_("Ease Out"), N_("Fade trails quickly, then approach zero gently")},
    {5, "CUSTOM", 0, N_("Custom"), N_("Edit the shutter's contribution over the full exposure")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();

  b.add_input<decl::Vector>("Speed"_ustr)
      .dimensions(4)
      .default_value({0.0f, 0.0f, 0.0f})
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_VELOCITY)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Depth"_ustr, "Z"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Int>("Samples"_ustr)
      .default_value(32)
      .min(1)
      .max(256)
      .description(
          "Samples per shutter half. Higher values improve directional filtering and "
          "curved-path accuracy");
  b.add_input<decl::Float>("Shutter"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .description("Time between shutter opening and closing in frames");
  b.add_input<decl::Bool>("Curved"_ustr)
      .default_value(false)
      .description("Interpolate between frames in a Bézier curve, rather than linearly");
  b.add_input<decl::Vector>("Speed Extended"_ustr)
      .dimensions(4)
      .default_value({0.0f, 0.0f, 0.0f})
      .subtype(PROP_VELOCITY)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description(
          "Optional Vector Extended render pass, holding the offsets two frames away. When "
          "connected, curved interpolation uses five frame positions to estimate a smooth "
          "motion path. Without this input, it uses three positions");
  b.add_input<decl::Menu>("Falloff"_ustr)
      .default_value(MenuValue(int(FalloffType::Linear)))
      .static_items(falloff_items)
      .description(
          "Fade along each pixel's own motion path. None uses uniform exposure; other shapes "
          "fade toward each shutter endpoint");
  b.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    const bNode &node = *ptr->data_as<bNode>();
    const bNodeSocket *falloff = bke::node_find_socket(node, SOCK_IN, "Falloff"_ustr);
    if (falloff->default_value_typed<bNodeSocketValueMenu>()->value != int(FalloffType::Custom)) {
      return;
    }
    layout.label("Shutter Curve", ICON_NONE);
    ui::template_curve_mapping(
        &layout, ptr, "shutter_curve", 0, false, false, false, false, false);
    ui::Layout &row = layout.row(true);
    row.prop(ptr, "shutter_curve_preset", ui::ITEM_R_EXPAND | ui::ITEM_R_ICON_ONLY, "", ICON_NONE);
    ui::Layout &labels = layout.row(false);
    labels.label("Previous", ICON_NONE);
    labels.label("Current", ICON_NONE);
    labels.label("Next", ICON_NONE);
  });
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  CurveMapping *mapping = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  mapping->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
  BKE_curvemap_reset(
      mapping->cm, &mapping->clipr, CURVE_PRESET_LINE, CurveMapSlopeType::PositiveNegative);
  BKE_curvemapping_changed(mapping, false);
  node->storage = mapping;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  /* Existing Vector Blur nodes have no curve storage. Preserve their saved Falloff. */
  if (!node->storage) {
    node_init(tree, node);
  }
}

using namespace blender::compositor;

/* Identical to the MotionPath in gpu_shader_compositor_motion_blur_lib.glsl. A motion path in
 * pixel space, relative to its source pixel:
 *
 *   offset(t) = tangent * t + curvature * t * t + cubic * t * t * t,   for t in [0, 1]
 *
 * For linear interpolation the tangent is the motion of the selected shutter half and the
 * curvature is zero, which reproduces the straight path offset(t) = motion * t exactly.
 * Curved interpolation uses a quadratic through three frame positions, or two cubic Hermite
 * segments when the two outer frame positions are available. */
struct MotionPath {
  /* Shutter-scaled displacement used as the tangent for a straight path. */
  float2 motion;
  float2 tangent;
  float2 curvature;
  float2 cubic;
};

/* Builds one shutter half's motion path from the source pixel's shutter-scaled motion.
 *
 * The path is the cubic Hermite segment through the current and the one frame away position of
 * the selected side, whose tangents are the central differences of their own neighbors. The
 * neighbor beyond the one frame away position comes from the extended motion, which holds the
 * offsets two frames out. Without it, the two frames out position is chosen so that the cubic
 * term vanishes, which leaves exactly the quadratic through the three available samples. */
static MotionPath compute_motion_path(const float4 &motion,
                                      const float4 &extended,
                                      const bool next,
                                      const bool use_curved,
                                      const bool use_extended,
                                      const float shutter_speed)
{
  MotionPath path;
  path.motion = next ? motion.zw() : motion.xy();
  path.tangent = path.motion;
  path.curvature = float2(0.0f);
  path.cubic = float2(0.0f);
  if (!use_curved) {
    return path;
  }

  /* Offsets to the neighboring positions of the selected side and of the opposite side. */
  const float2 near_offset = next ? motion.zw() : motion.xy();
  const float2 far_offset = next ? motion.xy() : motion.zw();
  const float2 outer_offset = use_extended ? (next ? extended.zw() : extended.xy()) :
                                             (far_offset + near_offset * 3.0f);

  /* Velocity at the current time. Shared by both sides, so the two halves of the path meet
   * without a kink at the current position. */
  path.tangent = (near_offset - far_offset) * 0.5f;
  /* Note the coefficients cancel for constant velocity, where the offsets are symmetric, leaving
   * the path exactly linear. */
  path.curvature = (far_offset + near_offset * 2.0f - outer_offset * 0.5f) * shutter_speed;
  path.cubic = (outer_offset - far_offset - near_offset * 3.0f) *
               (shutter_speed * shutter_speed * 0.5f);
  return path;
}

/* Position along the path at t in [0, 1], as an offset in pixels from the pixel it belongs to. */
static float2 motion_path_offset(const MotionPath &path, const float t)
{
  return ((path.cubic * t + path.curvature) * t + path.tangent) * t;
}

/* End of the path, which is where the straight chord ends in the linear case as well. */
static float2 motion_path_endpoint(const MotionPath &path)
{
  return path.tangent + path.curvature + path.cubic;
}

/* Upper bound of the distance in pixels between the path and its chord. The deviation is
 * curvature * (t^2 - t) + cubic * (t^3 - t), whose factors peak at 1/4 and 2/(3*sqrt(3)) over
 * [0, 1]. Together with the endpoint, this bounds movement when classifying source pixels. */
static float motion_path_deviation(const MotionPath &path)
{
  return math::length(path.curvature) * 0.25f + math::length(path.cubic) * 0.3849f;
}

/* Conservative distance from the current position, including paths that return to it. */
static float motion_path_reach(const MotionPath &path)
{
  return math::length(motion_path_endpoint(path)) + motion_path_deviation(path);
}

static float motion_blur_falloff(const FalloffType type, const float time)
{
  const float t = math::clamp(time, 0.0f, 1.0f);
  switch (type) {
    case FalloffType::Linear:
      return 1.0f - t;
    case FalloffType::Smooth:
      return 1.0f - t * t * (3.0f - 2.0f * t);
    case FalloffType::EaseIn:
      return 1.0f - t * t;
    case FalloffType::EaseOut:
      return (1.0f - t) * (1.0f - t);
    case FalloffType::None:
    case FalloffType::Custom:
      return 1.0f;
  }
  return 1.0f;
}

/* Midpoint quadrature integrates the linear and smooth windows exactly. Account for the
 * small quadratic midpoint error so a source's energy is independent of its sample count. */
static float exposure_window_average(const FalloffType type, const int count)
{
  switch (type) {
    case FalloffType::None:
    case FalloffType::Custom:
      return 1.0f;
    case FalloffType::Linear:
    case FalloffType::Smooth:
      return 0.5f;
    case FalloffType::EaseIn:
      return 2.0f / 3.0f + 1.0f / (12.0f * float(count) * float(count));
    case FalloffType::EaseOut:
      return 1.0f / 3.0f - 1.0f / (12.0f * float(count) * float(count));
  }
  return 1.0f;
}

/* Bound the maximum derivative, not just the endpoint or average path length. This keeps
 * consecutive source samples at most half a pixel apart, including returning cubic paths. */
static int exposure_samples(const MotionPath &path, const int segments)
{
  const float derivative_bound = math::length(path.tangent) + 2.0f * math::length(path.curvature) +
                                 3.0f * math::length(path.cubic);
  const int subdivisions = math::clamp(
      int(math::ceil(math::min(derivative_bound * 2.0f, 8192.0f) / float(segments))),
      1,
      8192 / segments);
  return segments * subdivisions;
}

/* Samples controls the curve's approximation. Spatial coverage is filled along each chord,
 * independently of that setting, so reducing geometric quality does not introduce empty slices. */
static float2 sampled_motion_path_offset(const MotionPath &path,
                                         const int segments,
                                         const int sample,
                                         const int count)
{
  const int subdivisions = count / segments;
  const int segment = sample / subdivisions;
  const float factor = (float(sample % subdivisions) + 0.5f) / float(subdivisions);
  const float2 start = motion_path_offset(path, float(segment) / float(segments));
  const float2 end = motion_path_offset(path, float(segment + 1) / float(segments));
  return math::interpolate(start, end, factor);
}

static void exposure_splat_cpu(const Result &input,
                               const Result &depth,
                               const Result &seeds,
                               MutableSpan<float4> accumulation,
                               const int2 &size,
                               const float2 &position,
                               const float4 &color,
                               const float source_depth)
{
  const int2 lower = int2(math::floor(position));
  const float2 fraction = position - float2(lower);
  for (int j = 0; j < 2; j++) {
    for (int i = 0; i < 2; i++) {
      const int2 target = lower + int2(i, j);
      if (target.x < 0 || target.y < 0 || target.x >= size.x || target.y >= size.y) {
        continue;
      }
      /* An opaque stationary surface can occlude a farther moving contribution. No depth
       * comparison is made against the moving original that is being replaced by exposure. */
      if (source_depth > 0.0f && seeds.load_pixel<int2, true>(target) == target &&
          input.load_pixel<Color>(target).a >= 1.0f)
      {
        const float target_depth = depth.load_pixel<float, true>(target);
        if (target_depth > 0.0f && source_depth > target_depth + 0.01f) {
          continue;
        }
      }
      const float coverage = (i ? fraction.x : 1.0f - fraction.x) *
                             (j ? fraction.y : 1.0f - fraction.y);
      const float4 contribution = color * coverage;
      float4 &pixel = accumulation[int64_t(target.y) * size.x + target.x];
      for (int channel = 0; channel < 4; channel++) {
        if (contribution[channel] != 0.0f) {
          std::atomic_ref<float>(pixel[channel])
              .fetch_add(contribution[channel], std::memory_order_relaxed);
        }
      }
    }
  }
}

/* Fixed exposure normalization is essential: normalizing only the moving contributions
 * would restore their full brightness at the original position and erase the fade. */
static float4 resolve_exposure(const float4 &sum,
                               const float4 &original,
                               const float4 &background,
                               const bool has_background)
{
  if (!has_background && original.w >= 1.0f && sum.w > 1e-8f) {
    /* Fully moving opaque images, such as camera motion, have no stationary background
     * seeds. Normalize spatial coverage to avoid making the opaque image transparent. */
    return float4(sum.xyz() / sum.w, 1.0f);
  }
  const float opacity = math::clamp(sum.w, 0.0f, 1.0f);
  const float4 foreground = sum / math::max(sum.w, 1.0f);
  return foreground + background * (1.0f - opacity);
}

class VectorBlurOperation : public NodeOperation {
 private:
  std::optional<ShutterCurve> shutter_curve_;

  const ShutterCurve &get_shutter_curve()
  {
    if (!shutter_curve_) {
      shutter_curve_.emplace(static_cast<const CurveMapping *>(node().storage));
    }
    return *shutter_curve_;
  }

 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    shutter_curve_.reset();
    Result &input = get_input("Image");
    if (input.is_single_value() || get_shutter() == 0.0f) {
      get_result("Image").share_data(input);
      return;
    }
    if (!get_use_curved()) {
      execute_linear();
      return;
    }
    if (context().use_gpu()) {
      execute_gpu();
    }
    else {
      execute_cpu();
    }
  }

  void execute_linear()
  {
    linear::blur(context(),
                 get_input("Image"),
                 get_input("Z"),
                 get_input("Speed"),
                 get_result("Image"),
                 compute_domain(),
                 get_samples_count(),
                 get_shutter(),
                 int(get_falloff()),
                 get_shutter_curve());
  }

  void execute_cpu()
  {
    const Result &input = get_input("Image");
    const Result &velocity = get_input("Speed");
    const Result &extended = get_input("Speed Extended");
    const Result &depth = get_input("Z");
    const Domain domain = compute_domain();
    const int2 size = domain.data_size;
    const int64_t pixels = int64_t(size.x) * size.y;
    const float shutter = get_shutter();
    const float4 mask(float2(shutter), float2(-shutter));
    const bool curved = get_use_curved();
    const bool use_extended = get_use_extended();
    const FalloffType falloff = get_falloff();
    const ShutterCurve &curve = get_shutter_curve();
    const int minimum_samples = get_samples_count();
    const bool reconstruct_background = !velocity.meta_data.is_estimated_motion;

    Result seeds = context().create_result(ResultType::Int2, ResultPrecision::Half);
    if (reconstruct_background) {
      seeds.allocate_texture(domain);
    }
    else {
      seeds.allocate_single_value();
      seeds.set_single_value(JUMP_FLOODING_NON_FLOODED_VALUE);
    }
    Array<int64_t> sources(pixels);
    std::atomic<int64_t> sources_count(0);
    std::atomic<bool> nonlinear(false);
    parallel_for(size, [&](const int2 texel) {
      const float4 color = float4(input.load_pixel<Color>(texel));
      const float4 motion = velocity.load_pixel<float4, true>(texel) * mask;
      const float4 outer = use_extended ? extended.load_pixel<float4, true>(texel) * mask :
                                          float4(0.0f);
      const MotionPath previous = compute_motion_path(
          motion, outer, false, curved, use_extended, shutter);
      const MotionPath next = compute_motion_path(
          motion, outer, true, curved, use_extended, shutter);
      if (color != float4(0.0f) &&
          math::max(math::length(previous.curvature) + math::length(previous.cubic),
                    math::length(next.curvature) + math::length(next.cubic)) > 1e-4f)
      {
        nonlinear.store(true, std::memory_order_relaxed);
      }
      const bool moving = color != float4(0.0f) &&
                          math::max(motion_path_reach(previous), motion_path_reach(next)) >= 0.5f;
      if (reconstruct_background) {
        seeds.store_pixel(texel, initialize_jump_flooding_value(texel, !moving));
      }
      if (moving) {
        const int64_t index = sources_count.fetch_add(1, std::memory_order_relaxed);
        sources[index] = int64_t(texel.y) * size.x + texel.x;
      }
    });
    if (sources_count == 0) {
      seeds.release();
      get_result("Image").share_data(input);
      return;
    }
    if (!nonlinear) {
      seeds.release();
      execute_linear();
      return;
    }

    Result flooded = context().create_result(ResultType::Int2, ResultPrecision::Half);
    if (reconstruct_background) {
      jump_flooding(context(), seeds, flooded);
    }
    else {
      flooded.share_data(seeds);
    }
    Array<float4> accumulation(pixels, float4(0.0f));
    threading::parallel_for(IndexRange(sources_count.load()), 16, [&](const IndexRange range) {
      for (const int64_t index : range) {
        const int64_t source = sources[index];
        const int2 texel(source % size.x, source / size.x);
        const float4 color = float4(input.load_pixel<Color>(texel));
        const float source_depth = depth.load_pixel<float, true>(texel);
        const float4 motion = velocity.load_pixel<float4, true>(texel) * mask;
        const float4 outer = use_extended ? extended.load_pixel<float4, true>(texel) * mask :
                                            float4(0.0f);
        for (const bool next : {false, true}) {
          const MotionPath path = compute_motion_path(
              motion, outer, next, curved, use_extended, shutter);
          const int count = exposure_samples(path, minimum_samples);
          const float window_average = falloff == FalloffType::Custom ?
                                           curve.average() :
                                           exposure_window_average(falloff, count);
          const float normalization = 0.5f / (float(count) * math::max(window_average, 1e-8f));
          for (int sample = 0; sample < count; sample++) {
            const float time = (float(sample) + 0.5f) / float(count);
            const float2 position = float2(texel) + sampled_motion_path_offset(
                                                        path, minimum_samples, sample, count);
            const float weight = falloff == FalloffType::Custom ?
                                     curve.mean(next,
                                                float(sample) / float(count),
                                                float(sample + 1) / float(count)) :
                                     motion_blur_falloff(falloff, time);
            const float4 weighted_color = color * (normalization * weight);
            exposure_splat_cpu(
                input, depth, seeds, accumulation, size, position, weighted_color, source_depth);
          }
        }
      }
    });
    Result &output = get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const int2 seed = flooded.load_pixel<int2, true>(texel);
      const bool has_background = seed != JUMP_FLOODING_NON_FLOODED_VALUE;
      const float4 original = float4(input.load_pixel<Color>(texel));
      /* With estimated motion, keep the current pixel for uncovered samples. */
      float4 background = reconstruct_background ? float4(0.0f) : original;
      if (has_background) {
        background = float4(input.load_pixel<Color>(seed));
      }
      const float4 sum = accumulation[int64_t(texel.y) * size.x + texel.x];
      output.store_pixel(texel,
                         Color(resolve_exposure(sum, original, background, has_background)));
    });
    flooded.release();
    seeds.release();
  }

  void bind_motion_settings(gpu::Shader *shader)
  {
    GPU_shader_uniform_1i(shader, "samples_count", get_samples_count());
    GPU_shader_uniform_1f(shader, "shutter_speed", get_shutter());
    GPU_shader_uniform_1b(shader, "use_curved_interpolation", get_use_curved());
    GPU_shader_uniform_1b(shader, "use_extended_motion", get_use_extended());
  }

  void execute_gpu()
  {
    Result &input = get_input("Image");
    Result &velocity = get_input("Speed");
    Result &extended = get_input("Speed Extended");
    Result &depth = get_input("Z");
    const Domain domain = compute_domain();
    const int64_t pixels = int64_t(domain.data_size.x) * domain.data_size.y;
    const bool reconstruct_background = !velocity.meta_data.is_estimated_motion;
    gpu::StorageBuf *statistics = GPU_storagebuf_create_ex(
        sizeof(uint32_t) * 4, nullptr, GPU_USAGE_DEVICE_ONLY, __func__);
    gpu::StorageBuf *sources = GPU_storagebuf_create_ex(
        sizeof(uint32_t) * pixels, nullptr, GPU_USAGE_DEVICE_ONLY, __func__);
    GPU_storagebuf_clear_to_zero(statistics);
    Result seeds = context().create_result(ResultType::Int2, ResultPrecision::Half);
    seeds.allocate_texture(domain);
    gpu::Shader *shader = context().get_shader("compositor_motion_blur_exposure_prepare");
    GPU_shader_bind(shader);
    bind_motion_settings(shader);
    GPU_shader_uniform_1b(shader, "reconstruct_background", reconstruct_background);
    input.bind_as_texture(shader, "input_tx");
    gpu::Texture *velocity_texture = velocity.bind_as_texture_or_single_value(shader,
                                                                              "velocity_tx");
    gpu::Texture *extended_texture = extended.bind_as_texture_or_single_value(
        shader, "velocity_extended_tx");
    GPU_storagebuf_bind(statistics, GPU_shader_get_ssbo_binding(shader, "statistics_buf"));
    GPU_storagebuf_bind(sources, GPU_shader_get_ssbo_binding(shader, "sources_buf"));
    seeds.bind_as_image(shader, "seeds_img");
    compute_dispatch_threads_at_least(shader, domain.data_size);
    GPU_shader_unbind();
    input.unbind_as_texture();
    velocity.unbind_as_texture_or_single_value(velocity_texture);
    extended.unbind_as_texture_or_single_value(extended_texture);
    seeds.unbind_as_image();
    /* The CPU reads statistics while later shaders read the compact source list. */
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_BUFFER_UPDATE);
    uint32_t info[4];
    GPU_storagebuf_read(statistics, info);
    GPU_storagebuf_free(statistics);
    if (info[0] == 0) {
      GPU_storagebuf_free(sources);
      seeds.release();
      get_result("Image").share_data(input);
      return;
    }
    if (info[2] == 0) {
      GPU_storagebuf_free(sources);
      seeds.release();
      execute_linear();
      return;
    }
    Result flooded = context().create_result(ResultType::Int2, ResultPrecision::Half);
    if (reconstruct_background) {
      jump_flooding(context(), seeds, flooded);
    }
    else {
      flooded.share_data(seeds);
    }
    gpu::StorageBuf *accumulation = GPU_storagebuf_create_ex(
        sizeof(float4) * pixels, nullptr, GPU_USAGE_DEVICE_ONLY, __func__);
    GPU_storagebuf_clear_to_zero(accumulation);
    shader = context().get_shader("compositor_motion_blur_exposure_accumulate");
    GPU_shader_bind(shader);
    bind_motion_settings(shader);
    GPU_shader_uniform_1i(shader, "falloff_type", int(get_falloff()));
    GPU_shader_uniform_1i(shader, "sources_count", int(info[0]));
    const Span<float2> curve_table = get_shutter_curve().table();
    gpu::StorageBuf *curve_buffer = GPU_storagebuf_create_ex(
        curve_table.size_in_bytes(), curve_table.data(), GPU_USAGE_STATIC, __func__);
    GPU_storagebuf_bind(curve_buffer, GPU_shader_get_ssbo_binding(shader, "shutter_curve_buf"));
    input.bind_as_texture(shader, "input_tx");
    velocity_texture = velocity.bind_as_texture_or_single_value(shader, "velocity_tx");
    extended_texture = extended.bind_as_texture_or_single_value(shader, "velocity_extended_tx");
    gpu::Texture *depth_texture = depth.bind_as_texture_or_single_value(shader, "depth_tx");
    seeds.bind_as_texture(shader, "seeds_tx");
    GPU_storagebuf_bind(sources, GPU_shader_get_ssbo_binding(shader, "sources_buf"));
    GPU_storagebuf_bind(accumulation, GPU_shader_get_ssbo_binding(shader, "accumulation_buf"));
    const int groups = (int(info[0]) + 63) / 64;
    const int groups_x = math::min(groups, 1024);
    const int groups_y = (groups + groups_x - 1) / groups_x;
    /* Bounded dispatches avoid a GPU watchdog timeout on dense or long motion fields. */
    for (int start = 0; start < int(info[1]); start += 8) {
      GPU_shader_uniform_1i(shader, "start_sample", start);
      GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
      GPU_compute_dispatch(shader, groups_x, groups_y, 1);
    }
    GPU_shader_unbind();
    input.unbind_as_texture();
    velocity.unbind_as_texture_or_single_value(velocity_texture);
    extended.unbind_as_texture_or_single_value(extended_texture);
    depth.unbind_as_texture_or_single_value(depth_texture);
    seeds.unbind_as_texture();
    GPU_storagebuf_free(sources);
    GPU_storagebuf_free(curve_buffer);
    seeds.release();

    shader = context().get_shader("compositor_motion_blur");
    GPU_shader_bind(shader);
    GPU_shader_uniform_1b(shader, "use_current_background", !reconstruct_background);
    input.bind_as_texture(shader, "input_tx");
    flooded.bind_as_texture(shader, "background_tx");
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
    GPU_storagebuf_bind(accumulation, GPU_shader_get_ssbo_binding(shader, "accumulation_buf"));
    Result &output = get_result("Image");
    output.allocate_texture(domain);
    output.bind_as_image(shader, "output_img");
    compute_dispatch_threads_at_least(shader, domain.data_size);
    GPU_shader_unbind();
    input.unbind_as_texture();
    flooded.unbind_as_texture();
    output.unbind_as_image();
    flooded.release();
    GPU_storagebuf_free(accumulation);
  }

  int get_samples_count()
  {
    return math::clamp(get_input("Samples").get_single_value_default<int>(), 1, 256);
  }

  FalloffType get_falloff()
  {
    return FalloffType(get_input("Falloff").get_single_value_default<MenuValue>().value);
  }

  float get_shutter()
  {
    return math::max(0.0f, get_input("Shutter").get_single_value_default<float>()) * 0.5f;
  }

  bool get_use_curved()
  {
    return get_input("Curved").get_single_value_default<bool>();
  }

  bool get_use_extended()
  {
    return get_use_curved() &&
           node().input_by_identifier("Speed Extended"_ustr)->is_logically_linked();
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new VectorBlurOperation(context, node);
}

/* Pair motion passes when the user connects a Render Layers Vector output. Explicit
 * Extended links are never replaced, and this does not enable an additional render pass. */
static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNodeLink &link = params.link;
  if (link.tonode != &params.node || !link.fromnode ||
      link.fromnode->typeinfo->idname != "CompositorNodeRLayers"_ustr ||
      !STREQ(link.tosock->identifier, "Speed") || !STREQ(link.fromsock->name, "Vector"))
  {
    return true;
  }
  params.ntree.ensure_topology_cache();
  bNodeSocket *input = params.node.input_by_identifier("Speed Extended"_ustr);
  bNodeSocket *output = link.fromnode->output_by_identifier("Vector Extended"_ustr);
  if (!input || !output || (output->flag & SOCK_UNAVAIL)) {
    return true;
  }
  for (const bNodeLink &existing : params.ntree.links) {
    if (existing.tosock == input) {
      return true;
    }
  }
  bke::node_add_link(params.ntree, *link.fromnode, *output, params.node, *input);
  return true;
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeVecBlur"_ustr, CMP_NODE_VECBLUR);
  ntype.ui_name = "Vector Blur";
  ntype.ui_description =
      "Blur the image along motion vectors from a render pass or estimated motion";
  ntype.enum_name_legacy = "VECBLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  /* Older files stored NodeBlurData here. Preserve its DNA type until legacy
   * versioning has consumed it, instead of reading it as a CurveMapping. */
  ntype.freefunc = node_free_curves;
  ntype.copyfunc = node_copy_curves;
  ntype.blend_write_storage_content =
      [](const bNodeTree &, const bNode &node, BlendWriter &writer) {
        if (node.storage) {
          writer.write_struct(static_cast<const CurveMapping *>(node.storage));
          BKE_curvemapping_curves_blend_write(&writer,
                                              static_cast<const CurveMapping *>(node.storage));
        }
      };
  ntype.blend_data_read_storage_content = [](bNodeTree &, bNode &node, BlendDataReader &reader) {
    if (node.storage && BLO_read_fileversion_get(&reader) >= 503) {
      BKE_curvemapping_blend_read(&reader, static_cast<CurveMapping *>(node.storage));
    }
  };
  ntype.insert_link = node_insert_link;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_vector_blur_cc
