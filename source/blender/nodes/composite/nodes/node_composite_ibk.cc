/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "RNA_enum_types.hh"

#include "GPU_shader.hh"

#include "COM_algorithm_keying.hh"
#include "COM_algorithm_symmetric_separable_blur.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_ibk_cc {

enum class ScreenChannel : int { Auto = 0, Green = 1, Blue = 2 };

static const EnumPropertyItem screen_items[] = {
    {int(ScreenChannel::Auto), "AUTO", 0, N_("Auto"), N_("Use the dominant clean plate channel")},
    {int(ScreenChannel::Green), "GREEN", 0, N_("Green"), N_("Key a green screen")},
    {int(ScreenChannel::Blue), "BLUE", 0, N_("Blue"), N_("Key a blue screen")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_output<decl::Float>("Matte"_ustr).structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Edges"_ustr)
      .structure_type(StructureType::Dynamic)
      .translation_context(BLT_I18NCONTEXT_ID_IMAGE);
  b.add_input<decl::Color>("Clean Plate"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Aligned image of the empty screen, in the same color space as the footage");
  b.add_input<decl::Menu>("Screen"_ustr)
      .default_value(ScreenChannel::Auto)
      .static_items(screen_items)
      .optional_label();

  PanelDeclarationBuilder &plate = b.add_panel("Plate"_ustr).default_closed(true);
  plate.add_input<decl::Float>("Exposure"_ustr, "Plate Exposure"_ustr)
      .default_value(0.0f)
      .min(-20.0f)
      .max(20.0f)
      .description("Adjust the clean plate exposure in stops to match the footage");
  plate.add_input<decl::Int>("Blur Size"_ustr, "Plate Blur Size"_ustr)
      .default_value(0)
      .min(0)
      .subtype(PROP_PIXEL)
      .description("Smooth noise in the clean plate before keying and screen subtraction");
  plate.add_input<decl::Float>("Balance"_ustr, "Screen Balance"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Weight the two non-screen channels when measuring screen color excess");

  PanelDeclarationBuilder &matte = b.add_panel("Matte"_ustr).default_closed(true);
  matte.add_input<decl::Int>("Input Blur Size"_ustr)
      .default_value(0)
      .min(0)
      .subtype(PROP_PIXEL)
      .description(
          "Smooth the footage only for matte calculation, preserving output image detail");
  matte.add_input<decl::Float>("Black Level"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Matte values below this level become transparent, except at detected edges");
  matte.add_input<decl::Float>("White Level"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Matte values above this level become opaque, except at detected edges");
  PanelDeclarationBuilder &edges = matte.add_panel("Edges"_ustr)
                                       .default_closed(true)
                                       .translation_context(BLT_I18NCONTEXT_ID_IMAGE);
  edges.add_input<decl::Int>("Size"_ustr, "Edge Search Size"_ustr)
      .default_value(3)
      .min(0)
      .subtype(PROP_PIXEL);
  edges.add_input<decl::Float>("Tolerance"_ustr, "Edge Tolerance"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);

  PanelDeclarationBuilder &masks = b.add_panel("Mask"_ustr).default_closed(true);
  masks.add_input<decl::Float>("Garbage Matte"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Exclude unwanted areas from the matte");
  masks.add_input<decl::Float>("Core Matte"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Include wanted areas in the matte");

  PanelDeclarationBuilder &cleanup = b.add_panel("Cleanup"_ustr).default_closed(true);
  cleanup.add_input<decl::Int>("Blur Size"_ustr, "Postprocess Blur Size"_ustr)
      .default_value(0)
      .min(0)
      .subtype(PROP_PIXEL);
  cleanup.add_input<decl::Int>("Dilate Size"_ustr, "Postprocess Dilate Size"_ustr)
      .default_value(0)
      .subtype(PROP_PIXEL)
      .description("Positive values expand the matte; negative values erode it");
  cleanup.add_input<decl::Int>("Feather Size"_ustr, "Postprocess Feather Size"_ustr)
      .default_value(0)
      .subtype(PROP_PIXEL);
  cleanup.add_input<decl::Menu>("Feather Falloff"_ustr)
      .default_value(PROP_SMOOTH)
      .static_items(rna_enum_proportional_falloff_curve_only_items)
      .optional_label()
      .translation_context(BLT_I18NCONTEXT_ID_CURVE_LEGACY);

  PanelDeclarationBuilder &despill = b.add_panel("Despill"_ustr).default_closed(true);
  despill.add_input<decl::Float>("Strength"_ustr, "Despill Strength"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Subtract the local screen contribution to recover foreground edge colors");
}

using namespace blender::compositor;

static constexpr float screen_epsilon = 1.0e-6f;

static int screen_channel(const float3 &screen, const ScreenChannel channel)
{
  if (channel != ScreenChannel::Auto) {
    return int(channel);
  }
  return screen.x > screen.y ? (screen.x > screen.z ? 0 : 2) : (screen.y > screen.z ? 1 : 2);
}

static float screen_excess(const float3 &color, const int channel, const float balance)
{
  const int first = (channel + 1) % 3;
  const int second = (channel + 2) % 3;
  return color[channel] - math::interpolate(color[math::min(first, second)],
                                            color[math::max(first, second)],
                                            balance);
}

static float3 screen_color(const float4 &plate, const float exposure)
{
  return plate.w > screen_epsilon ? plate.xyz() * (exposure / plate.w) : float3(0.0f);
}

static bool valid_screen(const float4 &plate,
                         const float3 &screen,
                         const int channel,
                         const float balance)
{
  /* A neutral, transparent, or extremely dark plate cannot determine screen coverage. Keep the
   * foreground there instead of dividing by zero or inventing a matte. */
  return plate.w > screen_epsilon &&
         screen_excess(screen, channel, balance) >
             screen_epsilon * math::max(1.0f, math::reduce_max(math::abs(screen)));
}

static float compute_matte_pixel(const float4 &image,
                                 const float4 &plate,
                                 const ScreenChannel channel,
                                 const float balance,
                                 const float exposure)
{
  if (image.w <= screen_epsilon) {
    return 0.0f;
  }
  const float3 screen = screen_color(plate, exposure);
  const int index = screen_channel(screen, channel);
  if (!valid_screen(plate, screen, index, balance)) {
    return 1.0f;
  }
  const float source_excess = screen_excess(image.xyz() / image.w, index, balance);
  const float plate_excess = screen_excess(screen, index, balance);
  return 1.0f - math::clamp(source_excess / plate_excess, 0.0f, 1.0f);
}

static float4 compute_image_pixel(const float4 &image,
                                  const float4 &plate,
                                  const float coverage,
                                  const ScreenChannel channel,
                                  const float balance,
                                  const float exposure,
                                  const float despill)
{
  const float alpha = math::clamp(image.w, 0.0f, 1.0f) * math::clamp(coverage, 0.0f, 1.0f);
  if (alpha <= 0.0f || image.w <= screen_epsilon) {
    return float4(0.0f);
  }
  const float3 screen = screen_color(plate, exposure);
  const int index = screen_channel(screen, channel);
  float3 color = image.xyz() * (alpha / image.w);
  if (valid_screen(plate, screen, index, balance)) {
    /* Solve C = a * F + (1 - a) * B for the premultiplied foreground a * F. This restores
     * translucent edge colors instead of simply darkening them by multiplying C by a. */
    const float3 foreground = image.xyz() - screen * (image.w - alpha);
    color = math::interpolate(color, foreground, despill);
  }
  return float4(color, alpha);
}

class IBKOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result source = blurred_input("Image", "Input Blur Size");
    Result plate = blurred_input("Clean Plate", "Plate Blur Size");
    Result raw_matte = compute_matte(source, plate);
    source.release();

    Result finished_matte = context().create_result(ResultType::Float);
    Result &output_edges = get_result("Edges");
    const bool needs_matte = get_result("Image").should_compute() ||
                             get_result("Matte").should_compute();
    keying_matte(context(),
                 raw_matte,
                 get_input("Garbage Matte"),
                 get_input("Core Matte"),
                 get_matte_settings(),
                 needs_matte ? &finished_matte : nullptr,
                 output_edges.should_compute() ? &output_edges : nullptr);
    raw_matte.release();
    if (needs_matte) {
      compute_output(plate, finished_matte);
      finished_matte.release();
    }
    plate.release();
  }

 private:
  float factor(const char *name) const
  {
    return math::clamp(get_input(name).get_single_value_default<float>(), 0.0f, 1.0f);
  }

  int size(const char *name) const
  {
    return math::max(0, get_input(name).get_single_value_default<int>());
  }

  ScreenChannel channel() const
  {
    return ScreenChannel(get_input("Screen").get_single_value_default<MenuValue>().value);
  }

  float exposure() const
  {
    return std::exp2(
        math::clamp(get_input("Plate Exposure").get_single_value_default<float>(), -20.0f, 20.0f));
  }

  KeyingMatteSettings get_matte_settings() const
  {
    KeyingMatteSettings settings;
    settings.black_level = factor("Black Level");
    settings.white_level = factor("White Level");
    settings.edge_search_radius = size("Edge Search Size");
    settings.edge_tolerance = factor("Edge Tolerance");
    settings.blur_size = size("Postprocess Blur Size");
    settings.dilate_size = get_input("Postprocess Dilate Size").get_single_value_default<int>();
    settings.feather_size = get_input("Postprocess Feather Size").get_single_value_default<int>();
    settings.feather_falloff =
        get_input("Feather Falloff").get_single_value_default<MenuValue>().value;
    return settings;
  }

  Result blurred_input(const char *input_name, const char *size_name)
  {
    Result &input = get_input(input_name);
    Result output = context().create_result(ResultType::Color);
    const int blur_size = size(size_name);
    if (blur_size == 0 || input.is_single_value()) {
      output.share_data(input);
    }
    else {
      symmetric_separable_blur(
          context(), input, output, float2(blur_size) / 2.0f, math::FilterKernel::Gauss);
    }
    return output;
  }

  void bind_screen_settings(gpu::Shader *shader) const
  {
    GPU_shader_uniform_1i(shader, "screen_channel", int(channel()));
    GPU_shader_uniform_1f(shader, "screen_balance", factor("Screen Balance"));
    GPU_shader_uniform_1f(shader, "plate_exposure", exposure());
  }

  Result compute_matte(const Result &source, const Result &plate)
  {
    Result output = context().create_result(ResultType::Float);
    if (source.is_single_value() && plate.is_single_value() &&
        get_input("Garbage Matte").is_single_value() && get_input("Core Matte").is_single_value())
    {
      output.allocate_single_value();
      output.set_single_value(compute_matte_pixel(float4(source.get_single_value<Color>()),
                                                  float4(plate.get_single_value<Color>()),
                                                  channel(),
                                                  factor("Screen Balance"),
                                                  exposure()));
      return output;
    }

    output.allocate_texture(compute_domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_ibk_compute_matte");
      GPU_shader_bind(shader);
      bind_screen_settings(shader);
      gpu::Texture *source_texture = source.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *plate_texture = plate.bind_as_texture_or_single_value(shader, "plate_tx");
      output.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, output.domain().data_size);
      GPU_shader_unbind();
      source.unbind_as_texture_or_single_value(source_texture);
      plate.unbind_as_texture_or_single_value(plate_texture);
      output.unbind_as_image();
    }
    else {
      const ScreenChannel screen = channel();
      const float balance = factor("Screen Balance");
      const float plate_exposure = exposure();
      parallel_for(output.domain().data_size, [&](const int2 texel) {
        output.store_pixel(texel,
                           compute_matte_pixel(float4(source.load_pixel<Color, true>(texel)),
                                               float4(plate.load_pixel<Color, true>(texel)),
                                               screen,
                                               balance,
                                               plate_exposure));
      });
    }
    return output;
  }

  void compute_output(const Result &plate, const Result &matte)
  {
    const Result &source = get_input("Image");
    Result &image_output = get_result("Image");
    Result &matte_output = get_result("Matte");
    const bool needs_image = image_output.should_compute();
    const ScreenChannel screen = channel();
    const float balance = factor("Screen Balance");
    const float plate_exposure = exposure();
    const float despill = factor("Despill Strength");

    if (matte.is_single_value()) {
      const float4 color = compute_image_pixel(float4(source.get_single_value<Color>()),
                                               float4(plate.get_single_value<Color>()),
                                               matte.get_single_value<float>(),
                                               screen,
                                               balance,
                                               plate_exposure,
                                               despill);
      if (needs_image) {
        image_output.allocate_single_value();
        image_output.set_single_value(Color(color));
      }
      if (matte_output.should_compute()) {
        matte_output.allocate_single_value();
        matte_output.set_single_value(color.w);
      }
      return;
    }

    Result final_matte = context().create_result(ResultType::Float);
    final_matte.allocate_texture(matte.domain());
    if (needs_image) {
      image_output.allocate_texture(matte.domain());
    }

    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader(needs_image ? "compositor_ibk_compute_image" :
                                                               "compositor_ibk_output_matte");
      GPU_shader_bind(shader);
      bind_screen_settings(shader);
      GPU_shader_uniform_1f(shader, "despill_strength", despill);
      gpu::Texture *source_texture = source.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *plate_texture = plate.bind_as_texture_or_single_value(shader, "plate_tx");
      matte.bind_as_texture(shader, "matte_tx");
      final_matte.bind_as_image(shader, "output_matte_img");
      if (needs_image) {
        image_output.bind_as_image(shader, "output_img");
      }
      compute_dispatch_threads_at_least(shader, matte.domain().data_size);
      GPU_shader_unbind();
      source.unbind_as_texture_or_single_value(source_texture);
      plate.unbind_as_texture_or_single_value(plate_texture);
      matte.unbind_as_texture();
      final_matte.unbind_as_image();
      if (needs_image) {
        image_output.unbind_as_image();
      }
    }
    else {
      parallel_for(matte.domain().data_size, [&](const int2 texel) {
        const float4 color = compute_image_pixel(float4(source.load_pixel<Color, true>(texel)),
                                                 float4(plate.load_pixel<Color, true>(texel)),
                                                 matte.load_pixel<float>(texel),
                                                 screen,
                                                 balance,
                                                 plate_exposure,
                                                 despill);
        final_matte.store_pixel(texel, color.w);
        if (needs_image) {
          image_output.store_pixel(texel, Color(color));
        }
      });
    }
    if (matte_output.should_compute()) {
      matte_output.share_data(final_matte);
    }
    final_matte.release();
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new IBKOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeIBK"_ustr);
  ntype.ui_name = "IBK";
  ntype.ui_description =
      "Image-based chroma keying and foreground color recovery using an aligned clean screen "
      "plate";
  ntype.nclass = NODE_CLASS_MATTE;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_ibk_cc
