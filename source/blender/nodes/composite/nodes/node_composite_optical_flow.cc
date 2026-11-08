/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "IMB_colormanagement.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_optical_flow_cc {

enum class Quality : int { Fast = 0, Balanced = 1, High = 2 };

static const EnumPropertyItem quality_items[] = {
    {int(Quality::Fast), "FAST", 0, N_("Fast"), N_("Smaller windows and fewer refinement steps")},
    {int(Quality::Balanced),
     "BALANCED",
     0,
     N_("Balanced"),
     N_("Balanced accuracy and processing cost")},
    {int(Quality::High),
     "HIGH",
     0,
     N_("High"),
     N_("Larger windows, deeper pyramids and more refinement")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(0)
      .description("First frame; motion vectors are measured at this image's pixels");
  b.add_output<decl::Vector>("Flow"_ustr)
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .align_with_previous()
      .description("Pixel displacement from Image to Next Image: positive X right, positive Y up");
  b.add_output<decl::Float>("Confidence"_ustr)
      .structure_type(StructureType::Dynamic)
      .description(
          "Heuristic reliability from texture, matching error and optional reverse consistency");
  b.add_input<decl::Color>("Next Image"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(1)
      .description("Second frame in the same color space, resolution and image alignment");
  b.add_input<decl::Menu>("Quality"_ustr)
      .default_value(Quality::Balanced)
      .static_items(quality_items)
      .optional_label();
  b.add_input<decl::Float>("Max Motion"_ustr)
      .default_value(64.0f)
      .min(0.0f)
      .max(2048.0f)
      .subtype(PROP_PIXEL)
      .description(
          "Maximum displacement length in output pixels; not a guarantee of capture range");
  b.add_input<decl::Float>("Mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(2)
      .description(
          "Suppress motion outside the mask; soft values weight confidence, not vector length");
  b.add_input<decl::Bool>("Consistency Check"_ustr)
      .default_value(true)
      .description("Estimate reverse motion too; only needed when Confidence is connected");
}

using namespace blender::compositor;

struct Settings {
  int levels;
  int iterations;
  int radius;
  float max_motion;
};

static constexpr float alpha_epsilon = 1.0e-6f;

static bool inside(const float2 &point, const int2 &size)
{
  return point.x >= 0.0f && point.y >= 0.0f && point.x <= size.x - 1 && point.y <= size.y - 1;
}

static float2 bounded(const float2 &flow, const float maximum)
{
  const float length = math::length(flow);
  return length > maximum && length > 0.0f ? flow * (maximum / length) : flow;
}

/* These helpers mirror gpu_shader_compositor_optical_flow.glsl. Internal levels contain
 * (premultiplied signed-log luminance, alpha, 0, 0); gradients contain (luminance, dx, dy, alpha).
 * Flow buffers contain (dx, dy, texture confidence, photometric confidence). */
static float4 sample(const Result &image, const float2 &point)
{
  const float2 floored = math::floor(point);
  const int2 base(floored);
  const float2 t = point - floored;
  return math::interpolate(math::interpolate(image.load_pixel_extended<float4>(base),
                                             image.load_pixel_extended<float4>(base + int2(1, 0)),
                                             t.x),
                           math::interpolate(image.load_pixel_extended<float4>(base + int2(0, 1)),
                                             image.load_pixel_extended<float4>(base + int2(1, 1)),
                                             t.x),
                           t.y);
}

static float luminance(const float4 &value)
{
  return value.y > alpha_epsilon ? value.x / value.y : 0.0f;
}

static float4 prepare_pixel(const float4 &color, const float3 &coefficients)
{
  if (!(color.w > alpha_epsilon) || !std::isfinite(color.w)) {
    return float4(0.0f);
  }
  const float value = math::dot(color.xyz() / color.w, coefficients);
  if (!std::isfinite(value)) {
    return float4(0.0f);
  }
  const float compressed = std::copysign(std::log(1.0f + std::abs(value)), value);
  const float alpha = math::min(color.w, 1.0f);
  return float4(compressed * alpha, alpha, 0.0f, 0.0f);
}

static float gaussian_weight(const int offset)
{
  return offset == 0 ? 6.0f : (std::abs(offset) == 1 ? 4.0f : 1.0f);
}

static float4 gradient_pixel(const Result &image, const int2 &point)
{
  const float4 center = image.load_pixel<float4>(point);
  const float4 left = image.load_pixel_extended<float4>(point - int2(1, 0));
  const float4 right = image.load_pixel_extended<float4>(point + int2(1, 0));
  const float4 down = image.load_pixel_extended<float4>(point - int2(0, 1));
  const float4 up = image.load_pixel_extended<float4>(point + int2(0, 1));
  const float alpha = math::min(center.y,
                                math::min(math::min(left.y, right.y), math::min(down.y, up.y)));
  return float4(luminance(center),
                0.5f * (luminance(right) - luminance(left)),
                0.5f * (luminance(up) - luminance(down)),
                alpha);
}

static float4 initial_pixel(const Result &gradient,
                            const Result &reference,
                            const int2 &point,
                            const int radius,
                            const float max_motion)
{
  if (gradient.load_pixel<float4>(point).w <= alpha_epsilon) {
    return float4(0.0f);
  }
  float total = 0.0f, mean = 0.0f, squared = 0.0f;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      const float4 a = gradient.load_pixel_zero<float4>(point + int2(x, y));
      total += a.w;
      mean += a.x * a.w;
      squared += a.x * a.x * a.w;
    }
  }
  const float variance = total > alpha_epsilon ?
                             math::max(0.0f, squared / total - (mean / total) * (mean / total)) :
                             0.0f;
  if (variance < 1.0e-8f) {
    return float4(0.0f);
  }
  /* Integer-only coarse seeds can prefer a distant periodic look-alike over the true
   * fractional displacement. Wide searches need quarter-pixel seeds; half pixels suffice
   * for the smaller capture radius at deeper pyramid levels. */
  const int substeps = max_motion > 4.0f ? 4 : 2;
  const int search_radius = substeps * math::min(8, int(std::ceil(max_motion)));
  float best_cost = std::numeric_limits<float>::max();
  float2 best(0.0f);
  for (int dy = -search_radius; dy <= search_radius; dy++) {
    for (int dx = -search_radius; dx <= search_radius; dx++) {
      const float2 candidate = float2(dx, dy) / float(substeps);
      if (math::length(candidate) > max_motion ||
          !inside(float2(point) + candidate, reference.domain().data_size))
      {
        continue;
      }
      float cost = 0.0f, weight = 0.0f;
      for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
          const int2 p = point + int2(x, y);
          const float2 q = float2(p) + candidate;
          if (!inside(q, reference.domain().data_size)) {
            continue;
          }
          const float4 a = gradient.load_pixel_zero<float4>(p);
          const float4 b = sample(reference, q);
          const float w = math::min(a.w, b.y);
          const float error = a.x - luminance(b);
          cost += w * error * error;
          weight += w;
        }
      }
      if (weight < 0.5f * total || weight <= alpha_epsilon) {
        continue;
      }
      cost /= weight;
      /* A weak, variance-normalized motion prior disambiguates repeating patches. Without
       * it, a distant look-alike can beat the slightly filtered true subpixel match. */
      cost += 0.005f * variance * math::length_squared(candidate);
      if (cost < best_cost - 1.0e-8f ||
          (std::abs(cost - best_cost) <= 1.0e-8f &&
           math::length_squared(candidate) < math::length_squared(best)))
      {
        best_cost = cost;
        best = candidate;
      }
    }
  }
  return float4(best.x, best.y, 0.0f, 0.0f);
}

static float4 iterate_pixel(const Result &gradient,
                            const Result &reference,
                            const Result &previous,
                            const int2 &point,
                            const int radius,
                            const float max_motion)
{
  const float4 old = previous.load_pixel<float4>(point);
  const float2 flow(old.x, old.y);
  if (gradient.load_pixel<float4>(point).w <= alpha_epsilon) {
    return float4(0.0f);
  }
  float xx = 0.0f, xy = 0.0f, yy = 0.0f, bx = 0.0f, by = 0.0f, weight = 0.0f, error_squared = 0.0f;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      const int2 p = point + int2(x, y);
      const float2 q = float2(p) + flow;
      if (!inside(q, reference.domain().data_size)) {
        continue;
      }
      const float4 a = gradient.load_pixel_zero<float4>(p);
      const float4 b = sample(reference, q);
      const float w = math::min(a.w, b.y);
      const float error = a.x - luminance(b);
      xx += w * a.y * a.y;
      xy += w * a.y * a.z;
      yy += w * a.z * a.z;
      bx += w * a.y * error;
      by += w * a.z * error;
      error_squared += w * error * error;
      weight += w;
    }
  }
  if (weight <= alpha_epsilon) {
    return float4(flow.x, flow.y, 0.0f, 0.0f);
  }
  const float trace = xx + yy;
  const float regularization = trace * 0.0001f + weight * 1.0e-8f;
  const float determinant = (xx + regularization) * (yy + regularization) - xy * xy;
  float2 delta(0.0f);
  if (trace > weight * 1.0e-7f && determinant > 1.0e-20f) {
    delta = float2((yy + regularization) * bx - xy * by, (xx + regularization) * by - xy * bx) /
            determinant;
  }
  const float2 refined = bounded(flow + bounded(delta, 1.0f), max_motion);
  const float condition = math::clamp(
      4.0f * math::max(0.0f, xx * yy - xy * xy) / math::max(trace * trace, 1.0e-20f), 0.0f, 1.0f);
  const float texture = trace / (trace + weight * 1.0e-5f);
  const float support = math::min(1.0f, weight / float((2 * radius + 1) * (2 * radius + 1)));
  const float photometric = 1.0f / (1.0f + error_squared / (weight * 0.0025f));
  return float4(refined.x, refined.y, condition * texture * support, photometric);
}

class OpticalFlowOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    if (!get_result("Flow").should_compute() && !get_result("Confidence").should_compute()) {
      return;
    }
    const Settings settings = get_settings();
    const Result &image = get_input("Image");
    const Result &next = get_input("Next Image");
    if (image.is_single_value() || next.is_single_value() || settings.max_motion == 0.0f ||
        math::reduce_min(compute_domain().data_size) < 3)
    {
      zero_outputs();
      if (image.is_single_value() || next.is_single_value()) {
        add_warning(NodeWarningType::Info,
                    "Optical Flow needs two spatial images with trackable detail");
      }
      return;
    }
    float3 coefficients;
    IMB_colormanagement_get_luminance_coefficients(coefficients);
    Vector<Result> first = build_pyramid(image, settings.levels, coefficients);
    Vector<Result> second = build_pyramid(next, settings.levels, coefficients);
    Result forward = estimate(first, second, settings);
    const bool consistency = get_result("Confidence").should_compute() &&
                             get_input("Consistency Check").get_single_value_default<bool>();
    Result backward = context().create_result(ResultType::Float4, ResultPrecision::Full);
    if (consistency) {
      backward = estimate(second, first, settings);
    }
    else {
      backward.allocate_single_value();
      backward.set_single_value(float4(0.0f));
    }
    output(first[0], second[0], forward, backward, consistency);
    forward.release();
    backward.release();
    for (Result &level : first) {
      level.release();
    }
    for (Result &level : second) {
      level.release();
    }
  }

 private:
  Settings get_settings() const
  {
    const Quality quality = Quality(
        get_input("Quality").get_single_value_default<MenuValue>().value);
    Settings settings = quality == Quality::Fast ? Settings{4, 3, 1, 0.0f} :
                        quality == Quality::High ? Settings{8, 8, 3, 0.0f} :
                                                   Settings{6, 5, 2, 0.0f};
    const float maximum = get_input("Max Motion").get_single_value_default<float>();
    settings.max_motion = std::isfinite(maximum) ? math::clamp(maximum, 0.0f, 2048.0f) : 0.0f;
    return settings;
  }

  void zero_outputs()
  {
    if (get_result("Flow").should_compute()) {
      get_result("Flow").allocate_single_value();
      get_result("Flow").set_single_value(float2(0.0f));
    }
    if (get_result("Confidence").should_compute()) {
      get_result("Confidence").allocate_single_value();
      get_result("Confidence").set_single_value(0.0f);
    }
  }

  Result prepare(const Result &image, const float3 &coefficients)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(compute_domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_prepare",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      GPU_shader_uniform_3fv(shader, "luminance_coefficients", coefficients);
      image.bind_as_texture(shader, "input_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size, [&](const int2 p) {
        result.store_pixel(p, prepare_pixel(float4(image.load_pixel<Color>(p)), coefficients));
      });
    }
    return result;
  }

  Result downsample(const Result &image)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(Domain(math::divide_ceil(image.domain().data_size, int2(2))));
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_downsample",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      image.bind_as_texture(shader, "input_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size, [&](const int2 p) {
        float4 sum(0.0f);
        for (int y = -2; y <= 2; y++) {
          for (int x = -2; x <= 2; x++) {
            sum += image.load_pixel_extended<float4>(p * 2 + int2(x, y)) *
                   (gaussian_weight(x) * gaussian_weight(y));
          }
        }
        result.store_pixel(p, sum / 256.0f);
      });
    }
    return result;
  }

  Vector<Result> build_pyramid(const Result &image,
                               const int maximum_levels,
                               const float3 &coefficients)
  {
    Vector<Result> levels;
    levels.append(prepare(image, coefficients));
    while (levels.size() < maximum_levels &&
           math::reduce_min(math::divide_ceil(levels.last().domain().data_size, int2(2))) >= 12)
    {
      /* Finish reading the previous level before append can reallocate the vector. */
      Result next = downsample(levels.last());
      levels.append(std::move(next));
    }
    return levels;
  }

  Result gradients(const Result &image)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(image.domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_gradients",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      image.bind_as_texture(shader, "input_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size,
                   [&](const int2 p) { result.store_pixel(p, gradient_pixel(image, p)); });
    }
    return result;
  }

  Result initialize(const Result &gradient,
                    const Result &reference,
                    const int radius,
                    const float maximum)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(gradient.domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_initialize",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      GPU_shader_uniform_1i(shader, "window_radius", radius);
      GPU_shader_uniform_1f(shader, "max_motion", maximum);
      gradient.bind_as_texture(shader, "gradient_tx");
      reference.bind_as_texture(shader, "reference_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      gradient.unbind_as_texture();
      reference.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size, [&](const int2 p) {
        result.store_pixel(p, initial_pixel(gradient, reference, p, radius, maximum));
      });
    }
    return result;
  }

  Result upsample(const Result &flow, const Domain &domain)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(domain);
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_upsample",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      flow.bind_as_texture(shader, "input_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      flow.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size, [&](const int2 p) {
        const float4 value = sample(flow, float2(p) * 0.5f);
        result.store_pixel(p, float4(2.0f * value.x, 2.0f * value.y, 0.0f, 0.0f));
      });
    }
    return result;
  }

  Result refine(const Result &gradient,
                const Result &reference,
                const Result &previous,
                const int radius,
                const float maximum)
  {
    Result result = context().create_result(ResultType::Float4, ResultPrecision::Full);
    result.allocate_texture(gradient.domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_optical_flow_iterate",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      GPU_shader_uniform_1i(shader, "window_radius", radius);
      GPU_shader_uniform_1f(shader, "max_motion", maximum);
      gradient.bind_as_texture(shader, "gradient_tx");
      reference.bind_as_texture(shader, "reference_tx");
      previous.bind_as_texture(shader, "flow_tx");
      result.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, result.domain().data_size);
      GPU_shader_unbind();
      gradient.unbind_as_texture();
      reference.unbind_as_texture();
      previous.unbind_as_texture();
      result.unbind_as_image();
    }
    else {
      parallel_for(result.domain().data_size, [&](const int2 p) {
        result.store_pixel(p, iterate_pixel(gradient, reference, previous, p, radius, maximum));
      });
    }
    return result;
  }

  Result estimate(const Vector<Result> &first,
                  const Vector<Result> &second,
                  const Settings &settings)
  {
    Result flow = context().create_result(ResultType::Float4, ResultPrecision::Full);
    for (int level = int(first.size()) - 1; level >= 0; level--) {
      const float maximum = settings.max_motion / float(1 << level);
      Result gradient = gradients(first[level]);
      if (level == first.size() - 1) {
        /* Tiny coarse patches can accept distant look-alike matches and propagate them to
         * every finer level. Keep at least 5x5 support even with Fast's 3x3 refinement. */
        flow = initialize(gradient, second[level], math::max(2, settings.radius), maximum);
      }
      else {
        Result enlarged = upsample(flow, first[level].domain());
        flow.release();
        flow = std::move(enlarged);
      }
      for (int iteration = 0; iteration < settings.iterations; iteration++) {
        Result refined = refine(gradient, second[level], flow, settings.radius, maximum);
        flow.release();
        flow = std::move(refined);
      }
      gradient.release();
    }
    return flow;
  }

  void output(const Result &first,
              const Result &second,
              const Result &forward,
              const Result &backward,
              const bool consistency)
  {
    Result &flow = get_result("Flow");
    Result &confidence = get_result("Confidence");
    const bool needs_flow = flow.should_compute();
    const bool needs_confidence = confidence.should_compute();
    const Domain domain = compute_domain();
    if (needs_flow) {
      flow.set_precision(ResultPrecision::Full);
      flow.allocate_texture(domain);
    }
    if (needs_confidence) {
      confidence.set_precision(ResultPrecision::Full);
      confidence.allocate_texture(domain);
    }
    const Result &mask = get_input("Mask");
    if (context().use_gpu()) {
      const char *name = needs_flow ? (needs_confidence ? "compositor_optical_flow_output_both" :
                                                          "compositor_optical_flow_output_flow") :
                                      "compositor_optical_flow_output_confidence";
      gpu::Shader *shader = context().get_shader(name, ResultPrecision::Full);
      GPU_shader_bind(shader);
      GPU_shader_uniform_1b(shader, "consistency_check", consistency);
      GPU_shader_uniform_2iv(shader, "output_size", domain.data_size);
      first.bind_as_texture(shader, "first_tx");
      second.bind_as_texture(shader, "second_tx");
      forward.bind_as_texture(shader, "forward_tx");
      gpu::Texture *backward_texture = backward.bind_as_texture_or_single_value(shader,
                                                                                "backward_tx");
      gpu::Texture *mask_texture = mask.bind_as_texture_or_single_value(shader, "mask_tx");
      if (needs_flow) {
        flow.bind_as_image(shader, "flow_img");
      }
      if (needs_confidence) {
        confidence.bind_as_image(shader, "confidence_img");
      }
      compute_dispatch_threads_at_least(shader, domain.data_size);
      GPU_shader_unbind();
      first.unbind_as_texture();
      second.unbind_as_texture();
      forward.unbind_as_texture();
      backward.unbind_as_texture_or_single_value(backward_texture);
      mask.unbind_as_texture_or_single_value(mask_texture);
      if (needs_flow) {
        flow.unbind_as_image();
      }
      if (needs_confidence) {
        confidence.unbind_as_image();
      }
      return;
    }
    parallel_for(domain.data_size, [&](const int2 p) {
      const float4 f = forward.load_pixel<float4>(p);
      float2 motion(f.x, f.y);
      float reliability = 0.0f;
      const float influence = math::clamp(mask.load_pixel<float, true>(p), 0.0f, 1.0f);
      const float2 q = float2(p) + motion;
      if (influence > 0.0f && first.load_pixel<float4>(p).y > alpha_epsilon &&
          inside(q, domain.data_size))
      {
        const float4 b = sample(second, q);
        if (b.y > alpha_epsilon) {
          const float error = luminance(first.load_pixel<float4>(p)) - luminance(b);
          reliability = f.z * f.w * influence / (1.0f + 100.0f * error * error);
          if (consistency) {
            const float4 reverse = sample(backward, q);
            const float2 back(reverse.x, reverse.y);
            const float tolerance = 1.0f + 0.01f * (math::length(motion) + math::length(back));
            reliability *= reverse.z > 0.0001f ?
                               1.0f / (1.0f + math::length_squared(motion + back) /
                                                  (tolerance * tolerance)) :
                               0.0f;
          }
        }
        else {
          motion = float2(0.0f);
        }
      }
      else {
        motion = float2(0.0f);
      }
      if (needs_flow) {
        flow.store_pixel(p, motion);
      }
      if (needs_confidence) {
        confidence.store_pixel(p, math::clamp(reliability, 0.0f, 1.0f));
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new OpticalFlowOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeOpticalFlow"_ustr);
  ntype.ui_name = "Optical Flow";
  ntype.ui_description = "Estimate dense pixel motion and confidence between two aligned images";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_optical_flow_cc
