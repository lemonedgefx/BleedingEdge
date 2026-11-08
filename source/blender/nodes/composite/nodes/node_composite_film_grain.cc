/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <cstdint>

#include "BLI_math_vector.hh"

#include "IMB_colormanagement.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_film_grain_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.18f, 0.18f, 0.18f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(0);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_input<decl::Float>("Amount"_ustr)
      .default_value(0.02f)
      .min(0.0f)
      .max(1.0f)
      .description("Grain standard deviation in scene-linear color, before tonal response");
  b.add_input<decl::Float>("Color"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Blend from monochrome grain to independently generated RGB grain");
  b.add_input<decl::Float>("Mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Where and how strongly to add grain");

  PanelDeclarationBuilder &sizes = b.add_panel("Grain Size"_ustr);
  sizes.add_input<decl::Float>("Red Size"_ustr)
      .default_value(1.0f)
      .min(0.5f)
      .max(64.0f)
      .subtype(PROP_PIXEL)
      .description("Red and monochrome grain size in output pixels");
  sizes.add_input<decl::Float>("Green Size"_ustr)
      .default_value(1.0f)
      .min(0.5f)
      .max(64.0f)
      .subtype(PROP_PIXEL)
      .description("Green grain size in output pixels");
  sizes.add_input<decl::Float>("Blue Size"_ustr)
      .default_value(1.0f)
      .min(0.5f)
      .max(64.0f)
      .subtype(PROP_PIXEL)
      .description("Blue grain size in output pixels");

  PanelDeclarationBuilder &response = b.add_panel("Tonal Response"_ustr).default_closed(true);
  response.add_input<decl::Float>("Shadows"_ustr).default_value(1.0f).min(0.0f).max(4.0f);
  response.add_input<decl::Float>("Midtones"_ustr).default_value(1.0f).min(0.0f).max(4.0f);
  response.add_input<decl::Float>("Highlights"_ustr).default_value(0.25f).min(0.0f).max(4.0f);
  response.add_input<decl::Float>("Pivot"_ustr)
      .default_value(0.18f)
      .min(0.0001f)
      .max(10.0f)
      .description("Scene-linear luminance at the center of the tonal transition");

  PanelDeclarationBuilder &animation = b.add_panel("Animation"_ustr).default_closed(true);
  animation.add_input<decl::Int>("Seed"_ustr).default_value(0);
  animation.add_input<decl::Bool>("Animate"_ustr)
      .default_value(true)
      .description("Use a deterministic new grain pattern on each scene frame");
}

using namespace blender::compositor;

/* Keep the integer hash and interpolation in sync with gpu_shader_compositor_film_grain.glsl.
 * Hashing integer coordinates avoids CPU/GPU differences from trigonometric random functions. */
static uint32_t grain_hash(uint32_t value)
{
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  value ^= value >> 16;
  return value;
}

static float grain_random(const uint32_t value)
{
  return float(grain_hash(value) >> 8) * (1.0f / 16777216.0f);
}

static float grain_cell(const int2 cell, const uint32_t seed)
{
  const uint32_t base = grain_hash(uint32_t(cell.x) ^ grain_hash(uint32_t(cell.y) + 0x9e3779b9U) ^
                                   seed);
  /* A centered, unit-variance approximation to Gaussian noise, bounded to avoid isolated spikes.
   */
  return (grain_random(base) + grain_random(base + 0x9e3779b9U) +
          grain_random(base + 0x3c6ef372U) + grain_random(base + 0xdaa66d2bU) - 2.0f) *
         1.7320508075688772f;
}

static float grain_noise(const float2 position, const float size, const uint32_t seed)
{
  const float2 coordinates = position / size - float2(0.5f);
  const float2 floored = math::floor(coordinates);
  const int2 cell(floored);
  const float2 fraction = coordinates - floored;
  const float2 t = fraction * fraction * (float2(3.0f) - 2.0f * fraction);
  const float4 weights(
      (1.0f - t.x) * (1.0f - t.y), t.x * (1.0f - t.y), (1.0f - t.x) * t.y, t.x * t.y);
  const float4 values(grain_cell(cell, seed),
                      grain_cell(cell + int2(1, 0), seed),
                      grain_cell(cell + int2(0, 1), seed),
                      grain_cell(cell + int2(1, 1), seed));
  /* Keep amount independent of subpixel phase and grain size. */
  return math::dot(weights, values) / std::sqrt(math::dot(weights, weights));
}

struct GrainSettings {
  float amount;
  float3 size;
  float color;
  float3 response;
  float pivot;
  uint32_t seed;
  float3 luminance_coefficients;
};

static float4 grain_pixel(const float4 &image,
                          const float mask,
                          const float2 &position,
                          const GrainSettings &settings)
{
  if (image.w <= 1.0e-6f || mask <= 0.0f || settings.amount == 0.0f) {
    return image;
  }
  const float luminance = math::max(
      0.0f, math::dot(image.xyz() / image.w, settings.luminance_coefficients));
  const float t = luminance / (luminance + settings.pivot);
  const float3 weights((1.0f - t) * (1.0f - t), 2.0f * t * (1.0f - t), t * t);
  const float response = math::dot(weights, settings.response);
  const float monochrome = grain_noise(position, settings.size.x, settings.seed);
  float3 noise(monochrome);
  if (settings.color > 0.0f) {
    const float3 colored(monochrome,
                         grain_noise(position, settings.size.y, grain_hash(settings.seed + 1U)),
                         grain_noise(position, settings.size.z, grain_hash(settings.seed + 2U)));
    noise = math::interpolate(noise, colored, settings.color);
  }
  const float scale = settings.amount * response * math::clamp(mask, 0.0f, 1.0f) * image.w;
  return float4(image.xyz() + noise * scale, image.w);
}

class FilmGrainOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    GrainSettings settings;
    settings.amount = math::max(0.0f, get_input("Amount").get_single_value_default<float>());
    settings.size = math::clamp(float3(get_input("Red Size").get_single_value_default<float>(),
                                       get_input("Green Size").get_single_value_default<float>(),
                                       get_input("Blue Size").get_single_value_default<float>()),
                                float3(0.5f),
                                float3(64.0f));
    settings.color = math::clamp(get_input("Color").get_single_value_default<float>(), 0.0f, 1.0f);
    settings.response = math::max(
        float3(get_input("Shadows").get_single_value_default<float>(),
               get_input("Midtones").get_single_value_default<float>(),
               get_input("Highlights").get_single_value_default<float>()),
        float3(0.0f));
    settings.pivot = math::max(0.0001f, get_input("Pivot").get_single_value_default<float>());
    settings.seed = uint32_t(get_input("Seed").get_single_value_default<int>());
    if (get_input("Animate").get_single_value_default<bool>()) {
      settings.seed ^= grain_hash(uint32_t(context().get_frame_number()));
    }
    /* The same non-negative bit pattern is passed through an integer GPU uniform. */
    settings.seed &= 0x7fffffffU;
    IMB_colormanagement_get_luminance_coefficients(settings.luminance_coefficients);

    Result &output = get_result("Image");
    const Result &image = get_input("Image");
    if (settings.amount == 0.0f || settings.response == float3(0.0f)) {
      output.share_data(image);
      return;
    }
    output.allocate_texture(compute_domain());
    const Result &mask = get_input("Mask");
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_film_grain");
      GPU_shader_bind(shader);
      GPU_shader_uniform_1f(shader, "amount", settings.amount);
      GPU_shader_uniform_3fv(shader, "grain_size", settings.size);
      GPU_shader_uniform_1f(shader, "color_amount", settings.color);
      GPU_shader_uniform_3fv(shader, "response", settings.response);
      GPU_shader_uniform_1f(shader, "pivot", settings.pivot);
      GPU_shader_uniform_1i(shader, "seed", int(settings.seed));
      GPU_shader_uniform_3fv(shader, "luminance_coefficients", settings.luminance_coefficients);
      GPU_shader_uniform_2iv(shader, "data_offset", output.domain().data_offset);
      gpu::Texture *image_texture = image.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *mask_texture = mask.bind_as_texture_or_single_value(shader, "mask_tx");
      output.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, output.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture_or_single_value(image_texture);
      mask.unbind_as_texture_or_single_value(mask_texture);
      output.unbind_as_image();
    }
    else {
      parallel_for(output.domain().data_size, [&](const int2 texel) {
        const float2 position = float2(texel + output.domain().data_offset) + float2(0.5f);
        output.store_pixel(texel,
                           Color(grain_pixel(float4(image.load_pixel<Color, true>(texel)),
                                             mask.load_pixel<float, true>(texel),
                                             position,
                                             settings)));
      });
    }
  }

  Domain compute_domain() override
  {
    if (get_input("Image").is_single_value() && get_input("Mask").is_single_value()) {
      /* Grain must be spatial even on a constant-color input. */
      return context().get_compositing_domain();
    }
    return NodeOperation::compute_domain();
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new FilmGrainOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeFilmGrain"_ustr);
  ntype.ui_name = "Film Grain";
  ntype.ui_description =
      "Add deterministic film grain or regrain with independent RGB sizes and tonal response";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_film_grain_cc
