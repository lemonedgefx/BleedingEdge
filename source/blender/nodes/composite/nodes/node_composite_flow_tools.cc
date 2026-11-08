/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_flow_tools_cc {

using namespace blender::compositor;

static const EnumPropertyItem fallback_items[] = {
    {0,
     "CROSSFADE",
     0,
     N_("Crossfade"),
     N_("Blend the original frames where motion is unreliable")},
    {1,
     "NEAREST",
     0,
     N_("Nearest Frame"),
     N_("Use the closer original frame where motion is unreliable")},
    {2, "TRANSPARENT", 0, N_("Transparent"), N_("Leave unreliable areas transparent")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void flow_input(NodeDeclarationBuilder &b, const UString name)
{
  b.add_input<decl::Vector>(name)
      .dimensions(2)
      .default_value({0.0f, 0.0f})
      .structure_type(StructureType::Dynamic);
}

static void confidence_input(NodeDeclarationBuilder &b, const UString name)
{
  b.add_input<decl::Float>(name)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic);
}

static void threshold_input(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Confidence Threshold"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Reject unreliable estimates without shortening accepted motion vectors");
}

static void declare_speed(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  flow_input(b, "Previous Flow"_ustr);
  b.add_output<decl::Vector>("Speed"_ustr)
      .dimensions(4)
      .structure_type(StructureType::Dynamic)
      .align_with_previous()
      .description("Vector Blur Speed: current-to-previous XY, negated current-to-next ZW");
  b.add_output<decl::Float>("Confidence"_ustr).structure_type(StructureType::Dynamic);
  flow_input(b, "Next Flow"_ustr);
  confidence_input(b, "Previous Confidence"_ustr);
  confidence_input(b, "Next Confidence"_ustr);
  threshold_input(b);
}

static void declare_visualize(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  flow_input(b, "Flow"_ustr);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  confidence_input(b, "Confidence"_ustr);
  b.add_input<decl::Float>("Display Range"_ustr)
      .default_value(64.0f)
      .min(0.001f)
      .max(2048.0f)
      .subtype(PROP_PIXEL)
      .description("Motion length shown at full brightness; hue shows direction");
}

static void declare_interpolate(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(0);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_output<decl::Float>("Confidence"_ustr).structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Next Image"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(1);
  flow_input(b, "Forward Flow"_ustr);
  flow_input(b, "Backward Flow"_ustr);
  confidence_input(b, "Forward Confidence"_ustr);
  confidence_input(b, "Backward Confidence"_ustr);
  b.add_input<decl::Float>("Factor"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Time between the supplied frames: zero is Image, one is Next Image");
  b.add_input<decl::Menu>("Fallback"_ustr)
      .default_value(MenuValue(0))
      .static_items(fallback_items);
  threshold_input(b);
}

static bool finite_motion(const float2 &value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && math::length(value) <= 2048.0f;
}

static float unit_confidence(const float value)
{
  return std::isfinite(value) ? math::clamp(value, 0.0f, 1.0f) : 0.0f;
}

template<typename T> static T sample_field(const Result &result, const float2 &point)
{
  if (result.is_single_value()) {
    return result.get_single_value<T>();
  }
  const int2 base(math::floor(point));
  const float2 t = point - float2(base);
  return math::interpolate(math::interpolate(result.load_pixel_extended<T>(base),
                                             result.load_pixel_extended<T>(base + int2(1, 0)),
                                             t.x),
                           math::interpolate(result.load_pixel_extended<T>(base + int2(0, 1)),
                                             result.load_pixel_extended<T>(base + int2(1, 1)),
                                             t.x),
                           t.y);
}

static bool inside(const float2 &point, const int2 &size)
{
  return point.x >= 0.0f && point.y >= 0.0f && point.x <= size.x - 1 && point.y <= size.y - 1;
}

static float4 sample_color(const Result &image, const float2 &point)
{
  if (image.is_single_value()) {
    return float4(image.get_single_value<Color>());
  }
  /* Mirror tools_sample on the GPU. Keep pixel coordinates and integer neighbours: both
   * UV round-trips and adding 1.0 to a float just below the border can round to the wrong
   * texel. Inverse-warp support has already rejected out-of-frame source coordinates. */
  const int2 base(math::floor(point));
  const float2 t = point - float2(base);
  return math::interpolate(
      math::interpolate(float4(image.load_pixel_extended<Color>(base)),
                        float4(image.load_pixel_extended<Color>(base + int2(1, 0))),
                        t.x),
      math::interpolate(float4(image.load_pixel_extended<Color>(base + int2(0, 1))),
                        float4(image.load_pixel_extended<Color>(base + int2(1, 1))),
                        t.x),
      t.y);
}

struct WarpSample {
  float2 point;
  float confidence;
};

/* Displace performs a backward gather. Forward flow is indexed at the source instead, so
 * merely multiplying it and plugging it into Displace is not a general inverse warp.
 * Solve source + time * flow(source) = destination, then gather with the existing sampler. */
static WarpSample inverse_warp(const Result &flow,
                               const Result &reverse,
                               const Result &confidence,
                               const float2 &destination,
                               const float time,
                               const int2 &size)
{
  float2 point = destination;
  for (int iteration = 0; iteration < 12; iteration++) {
    const float2 motion = sample_field<float2>(flow, point);
    if (!finite_motion(motion)) {
      return {destination, 0.0f};
    }
    point = destination - time * motion;
  }
  if (!inside(point, size)) {
    return {destination, 0.0f};
  }
  const float2 motion = sample_field<float2>(flow, point);
  const float2 end = point + motion;
  if (!finite_motion(motion) || !inside(end, size)) {
    return {destination, 0.0f};
  }
  const float2 back = sample_field<float2>(reverse, end);
  if (!finite_motion(back)) {
    return {destination, 0.0f};
  }
  const float residual = math::length_squared(point + time * motion - destination);
  const float tolerance = 1.0f + 0.01f * (math::length(motion) + math::length(back));
  const float consistency = 1.0f /
                            (1.0f + math::length_squared(motion + back) / (tolerance * tolerance));
  return {point,
          unit_confidence(sample_field<float>(confidence, point)) * consistency /
              (1.0f + residual)};
}

struct Binding {
  const char *socket;
  const char *shader;
};

class FlowToolsOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    if (node().idname == "CompositorNodeFlowToSpeed"_ustr) {
      execute_speed();
    }
    else if (node().idname == "CompositorNodeFlowVisualize"_ustr) {
      execute_visualize();
    }
    else {
      execute_interpolate();
    }
  }

 private:
  void dispatch(const char *shader_name,
                const Span<Binding> inputs,
                const Span<Binding> outputs,
                const float parameter,
                const float threshold = 0.0f,
                const int fallback = 0)
  {
    gpu::Shader *shader = context().get_shader(shader_name, ResultPrecision::Full);
    GPU_shader_bind(shader);
    GPU_shader_uniform_2iv(shader, "output_size", compute_domain().data_size);
    if (node().idname != "CompositorNodeFlowToSpeed"_ustr) {
      GPU_shader_uniform_1f(shader, "parameter", parameter);
    }
    if (node().idname != "CompositorNodeFlowVisualize"_ustr) {
      GPU_shader_uniform_1f(shader, "confidence_threshold", threshold);
    }
    if (node().idname == "CompositorNodeFlowInterpolate"_ustr) {
      GPU_shader_uniform_1i(shader, "fallback_mode", fallback);
    }
    Vector<gpu::Texture *> textures;
    for (const Binding &binding : inputs) {
      textures.append(
          get_input(binding.socket).bind_as_texture_or_single_value(shader, binding.shader));
    }
    for (const Binding &binding : outputs) {
      Result &result = get_result(binding.socket);
      if (result.should_compute()) {
        result.set_precision(ResultPrecision::Full);
        result.allocate_texture(compute_domain());
        result.bind_as_image(shader, binding.shader);
      }
    }
    compute_dispatch_threads_at_least(shader, compute_domain().data_size);
    GPU_shader_unbind();
    for (const int index : inputs.index_range()) {
      get_input(inputs[index].socket).unbind_as_texture_or_single_value(textures[index]);
    }
    for (const Binding &binding : outputs) {
      if (get_result(binding.socket).should_compute()) {
        get_result(binding.socket).unbind_as_image();
      }
    }
  }

  void allocate_cpu(const Span<const char *> outputs, const bool constant)
  {
    for (const char *name : outputs) {
      Result &result = get_result(name);
      if (result.should_compute()) {
        result.set_precision(ResultPrecision::Full);
        if (constant) {
          result.allocate_single_value();
        }
        else {
          result.allocate_texture(compute_domain());
        }
      }
    }
  }

  template<typename T> static void store_result(Result &result, const int2 &point, const T &value)
  {
    if (!result.should_compute()) {
      return;
    }
    if (result.is_single_value()) {
      result.set_single_value(value);
    }
    else {
      result.store_pixel(point, value);
    }
  }

  float threshold() const
  {
    return unit_confidence(get_input("Confidence Threshold").get_single_value_default<float>());
  }

  void execute_speed()
  {
    Result &speed = get_result("Speed");
    Result &confidence = get_result("Confidence");
    if (speed.should_compute()) {
      speed.meta_data.is_estimated_motion = true;
    }
    const bool constant = get_input("Previous Flow").is_single_value() &&
                          get_input("Next Flow").is_single_value() &&
                          get_input("Previous Confidence").is_single_value() &&
                          get_input("Next Confidence").is_single_value();
    if (context().use_gpu() && !constant) {
      const Binding inputs[] = {{"Previous Flow", "previous_tx"},
                                {"Next Flow", "next_tx"},
                                {"Previous Confidence", "previous_confidence_tx"},
                                {"Next Confidence", "next_confidence_tx"}};
      const Binding outputs[] = {{"Speed", "speed_img"}, {"Confidence", "confidence_img"}};
      dispatch(speed.should_compute() ?
                   (confidence.should_compute() ? "compositor_flow_to_speed_both" :
                                                  "compositor_flow_to_speed_speed") :
                   "compositor_flow_to_speed_confidence",
               {inputs, ARRAY_SIZE(inputs)},
               {outputs, ARRAY_SIZE(outputs)},
               0.0f,
               threshold());
      return;
    }
    const char *outputs[] = {"Speed", "Confidence"};
    allocate_cpu({outputs, ARRAY_SIZE(outputs)}, constant);
    parallel_for(constant ? int2(1) : compute_domain().data_size, [&](const int2 p) {
      const float2 previous = get_input("Previous Flow").load_pixel<float2, true>(p);
      const float2 next = get_input("Next Flow").load_pixel<float2, true>(p);
      const float a = finite_motion(previous) ?
                          unit_confidence(
                              get_input("Previous Confidence").load_pixel<float, true>(p)) :
                          0.0f;
      const float b = finite_motion(next) ?
                          unit_confidence(
                              get_input("Next Confidence").load_pixel<float, true>(p)) :
                          0.0f;
      const bool valid_a = a > 0.0f && a >= threshold();
      const bool valid_b = b > 0.0f && b >= threshold();
      if (speed.should_compute()) {
        store_result(
            speed, p, float4(valid_a ? previous : float2(0.0f), valid_b ? -next : float2(0.0f)));
      }
      if (confidence.should_compute()) {
        store_result(confidence, p, math::min(valid_a ? a : 0.0f, valid_b ? b : 0.0f));
      }
    });
  }

  void execute_visualize()
  {
    const float range = get_input("Display Range").get_single_value_default<float>();
    const float safe_range = std::isfinite(range) ? math::clamp(range, 0.001f, 2048.0f) : 64.0f;
    const bool constant = get_input("Flow").is_single_value() &&
                          get_input("Confidence").is_single_value();
    if (context().use_gpu() && !constant) {
      const Binding inputs[] = {{"Flow", "flow_tx"}, {"Confidence", "confidence_tx"}};
      const Binding outputs[] = {{"Image", "output_img"}};
      dispatch("compositor_flow_visualize",
               {inputs, ARRAY_SIZE(inputs)},
               {outputs, ARRAY_SIZE(outputs)},
               safe_range);
      return;
    }
    const char *outputs[] = {"Image"};
    allocate_cpu({outputs, ARRAY_SIZE(outputs)}, constant);
    parallel_for(constant ? int2(1) : compute_domain().data_size, [&](const int2 p) {
      const float2 flow = get_input("Flow").load_pixel<float2, true>(p);
      float3 color(0.0f);
      if (finite_motion(flow)) {
        const float hue = std::atan2(flow.y, flow.x) / 6.283185307179586f + 1.0f;
        const float value = math::min(1.0f, math::length(flow) / safe_range) *
                            unit_confidence(get_input("Confidence").load_pixel<float, true>(p));
        const float3 wheel = math::abs(
            math::fract(float3(hue) + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
        color = math::clamp(wheel - 1.0f, 0.0f, 1.0f) * value;
      }
      store_result(get_result("Image"), p, Color(float4(color, 1.0f)));
    });
  }

  void execute_interpolate()
  {
    const float input_factor = get_input("Factor").get_single_value_default<float>();
    const float factor = std::isfinite(input_factor) ? math::clamp(input_factor, 0.0f, 1.0f) :
                                                       0.0f;
    Result &image = get_result("Image");
    Result &confidence = get_result("Confidence");
    /* Exact endpoints must not be filtered or depend on flow validity. */
    if (factor == 0.0f || factor == 1.0f) {
      if (image.should_compute()) {
        image.share_data(get_input(factor == 0.0f ? "Image" : "Next Image"));
      }
      if (confidence.should_compute()) {
        confidence.allocate_single_value();
        confidence.set_single_value(1.0f);
      }
      return;
    }
    const int fallback = get_input("Fallback").get_single_value_default<MenuValue>().value;
    const bool constant = get_input("Image").is_single_value() &&
                          get_input("Next Image").is_single_value() &&
                          get_input("Forward Flow").is_single_value() &&
                          get_input("Backward Flow").is_single_value() &&
                          get_input("Forward Confidence").is_single_value() &&
                          get_input("Backward Confidence").is_single_value();
    if (context().use_gpu() && !constant) {
      const Binding inputs[] = {{"Image", "first_tx"},
                                {"Next Image", "second_tx"},
                                {"Forward Flow", "forward_tx"},
                                {"Backward Flow", "backward_tx"},
                                {"Forward Confidence", "forward_confidence_tx"},
                                {"Backward Confidence", "backward_confidence_tx"}};
      const Binding outputs[] = {{"Image", "output_img"}, {"Confidence", "confidence_img"}};
      dispatch(image.should_compute() ?
                   (confidence.should_compute() ? "compositor_flow_interpolate_both" :
                                                  "compositor_flow_interpolate_image") :
                   "compositor_flow_interpolate_confidence",
               {inputs, ARRAY_SIZE(inputs)},
               {outputs, ARRAY_SIZE(outputs)},
               factor,
               threshold(),
               fallback);
      return;
    }
    const char *outputs[] = {"Image", "Confidence"};
    allocate_cpu({outputs, ARRAY_SIZE(outputs)}, constant);
    const int2 size = constant ? int2(1) : compute_domain().data_size;
    parallel_for(size, [&](const int2 p) {
      const WarpSample a = inverse_warp(get_input("Forward Flow"),
                                        get_input("Backward Flow"),
                                        get_input("Forward Confidence"),
                                        float2(p),
                                        factor,
                                        size);
      const WarpSample b = inverse_warp(get_input("Backward Flow"),
                                        get_input("Forward Flow"),
                                        get_input("Backward Confidence"),
                                        float2(p),
                                        1.0f - factor,
                                        size);
      const float4 ca = sample_color(get_input("Image"), a.point);
      const float4 cb = sample_color(get_input("Next Image"), b.point);
      const auto supported = [](const float4 &color) {
        return color.w > 1.0e-6f && std::isfinite(color.x) && std::isfinite(color.y) &&
               std::isfinite(color.z) && std::isfinite(color.w);
      };
      const float wa = supported(ca) && a.confidence >= threshold() ?
                           (1.0f - factor) * a.confidence :
                           0.0f;
      const float wb = supported(cb) && b.confidence >= threshold() ? factor * b.confidence : 0.0f;
      const float total = wa + wb;
      if (image.should_compute()) {
        float4 color(0.0f);
        if (total > 1.0e-6f) {
          /* Do not multiply an unsupported (possibly NaN) sample by zero. */
          color = ((wa > 0.0f ? ca * wa : float4(0.0f)) + (wb > 0.0f ? cb * wb : float4(0.0f))) /
                  total;
        }
        else if (fallback == 0) {
          color = math::interpolate(float4(get_input("Image").load_pixel<Color, true>(p)),
                                    float4(get_input("Next Image").load_pixel<Color, true>(p)),
                                    factor);
        }
        else if (fallback == 1) {
          color = float4(
              get_input(factor < 0.5f ? "Image" : "Next Image").load_pixel<Color, true>(p));
        }
        store_result(image, p, Color(color));
      }
      if (confidence.should_compute()) {
        store_result(confidence, p, unit_confidence(total));
      }
    });
  }
};

static NodeOperation *get_operation(Context &context, const bNode &node)
{
  return new FlowToolsOperation(context, node);
}

static void register_speed()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeFlowToSpeed"_ustr);
  ntype.ui_name = "Flow to Speed";
  ntype.ui_description = "Pack independently estimated past and future motion for Vector Blur";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = declare_speed;
  ntype.get_compositor_operation = get_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_speed)

static void register_visualize()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeFlowVisualize"_ustr);
  ntype.ui_name = "Flow Visualize";
  ntype.ui_description =
      "Show motion direction as hue and motion length and confidence as brightness";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = declare_visualize;
  ntype.get_compositor_operation = get_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_visualize)

static void register_interpolate()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeFlowInterpolate"_ustr);
  ntype.ui_name = "Flow Interpolate";
  ntype.ui_description = "Interpolate two images using bidirectional source-indexed optical flow";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = declare_interpolate;
  ntype.get_compositor_operation = get_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_interpolate)

}  // namespace blender::nodes::node_composite_flow_tools_cc
