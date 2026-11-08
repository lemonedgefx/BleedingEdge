/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_relight_cc {

enum class LightType : int { Directional = 0, Point = 1 };

static const EnumPropertyItem light_items[] = {
    {int(LightType::Directional),
     "DIRECTIONAL",
     0,
     N_("Directional"),
     N_("A distant light with a constant direction")},
    {int(LightType::Point),
     "POINT",
     0,
     N_("Point"),
     N_("A local light, using the world-space Position pass")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(0);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_output<decl::Color>("Lighting"_ustr)
      .structure_type(StructureType::Dynamic)
      .description("Masked, premultiplied diffuse-light contribution, without the input image");
  b.add_input<decl::Vector>("Normal"_ustr)
      .default_value(float3(0.0f))
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(1)
      .description("Signed world-space Normal pass, not an RGB-encoded normal map");
  b.add_input<decl::Vector>("Position"_ustr)
      .default_value(float3(0.0f))
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(2)
      .description("World-space Position pass; only used for point lights");
  b.add_input<decl::Color>("Diffuse Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Unlit diffuse color or albedo, not the already shaded Diffuse Direct pass");
  b.add_input<decl::Float>("Mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &light = b.add_panel("Light"_ustr);
  light.add_input<decl::Menu>("Type"_ustr)
      .default_value(LightType::Directional)
      .static_items(light_items)
      .optional_label();
  light.add_input<decl::Color>("Color"_ustr, "Light Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
  light.add_input<decl::Float>("Strength"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(100.0f)
      .description("Diffuse-light multiplier, not Blender light power in watts");
  light.add_input<decl::Vector>("Direction"_ustr)
      .default_value({0.0f, 0.0f, 1.0f})
      .subtype(PROP_DIRECTION)
      .description("World-space direction from the surface toward a directional light");

  PanelDeclarationBuilder &point = b.add_panel("Point Light"_ustr).default_closed(true);
  point.add_input<decl::Vector>("Location"_ustr, "Light Position"_ustr)
      .default_value({0.0f, 0.0f, 5.0f})
      .subtype(PROP_TRANSLATION)
      .description("Light location in the same world space as the Position pass");
  point.add_input<decl::Bool>("Distance Falloff"_ustr)
      .default_value(true)
      .description("Use inverse-square attenuation for a point light");
  point.add_input<decl::Float>("Minimum Distance"_ustr)
      .default_value(0.1f)
      .min(0.0001f)
      .max(100.0f)
      .subtype(PROP_DISTANCE)
      .description("Clamp point-light distance to avoid singular brightness near the light");
}

using namespace blender::compositor;

struct RelightSettings {
  LightType type;
  float3 color;
  float strength;
  float3 direction;
  float3 position;
  bool distance_falloff;
  float minimum_distance;
};

static bool finite_vector(const float3 &value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

static float4 lighting_pixel(const float4 &image,
                             const float3 &normal,
                             const float3 &position,
                             const float4 &diffuse,
                             const float mask,
                             const RelightSettings &settings)
{
  const float4 unlit(0.0f, 0.0f, 0.0f, image.w);
  if (!(image.w > 1.0e-6f) || !(mask > 0.0f) || !finite_vector(normal)) {
    return unlit;
  }
  const float normal_length = math::length(normal);
  if (!(normal_length > 1.0e-6f) || !std::isfinite(normal_length)) {
    return unlit;
  }
  float3 direction = settings.direction;
  float attenuation = 1.0f;
  if (settings.type == LightType::Point) {
    if (!finite_vector(position)) {
      return unlit;
    }
    direction = settings.position - position;
    const float distance_squared = math::length_squared(direction);
    if (settings.distance_falloff) {
      attenuation = 1.0f / math::max(distance_squared,
                                     settings.minimum_distance * settings.minimum_distance);
    }
  }
  const float direction_length = math::length(direction);
  if (!(direction_length > 1.0e-6f) || !std::isfinite(direction_length)) {
    return unlit;
  }
  const float lambert = math::max(0.0f,
                                  math::dot(normal / normal_length, direction / direction_length));
  /* Diffuse Color inputs use the same premultiplication convention as other color sockets. */
  const float3 albedo = diffuse.w > 1.0e-6f ? diffuse.xyz() / diffuse.w : float3(0.0f);
  const float influence = settings.strength * lambert * attenuation *
                          math::clamp(mask, 0.0f, 1.0f) * image.w;
  return float4(albedo * settings.color * influence, image.w);
}

class RelightOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    RelightSettings settings;
    settings.type = LightType(get_input("Type").get_single_value_default<MenuValue>().value);
    settings.color = float4(get_input("Light Color").get_single_value_default<Color>()).xyz();
    settings.strength = math::max(0.0f, get_input("Strength").get_single_value_default<float>());
    settings.direction = get_input("Direction").get_single_value_default<float3>();
    settings.position = get_input("Light Position").get_single_value_default<float3>();
    settings.distance_falloff = get_input("Distance Falloff").get_single_value_default<bool>();
    settings.minimum_distance = math::max(
        0.0001f, get_input("Minimum Distance").get_single_value_default<float>());

    const Result &image = get_input("Image");
    const Result &normal = get_input("Normal");
    const Result &position = get_input("Position");
    const Result &diffuse = get_input("Diffuse Color");
    const Result &mask = get_input("Mask");
    if (normal.is_single_value() &&
        math::length_squared(normal.get_single_value<float3>()) <= 1.0e-12f)
    {
      add_warning(
          NodeWarningType::Info,
          "Connect a signed world-space Normal pass to Relight. Zero normals receive no light.");
    }

    if (image.is_single_value() && normal.is_single_value() && position.is_single_value() &&
        diffuse.is_single_value() && mask.is_single_value())
    {
      const float4 source(image.get_single_value<Color>());
      const float4 light = lighting_pixel(source,
                                          normal.get_single_value<float3>(),
                                          position.get_single_value<float3>(),
                                          float4(diffuse.get_single_value<Color>()),
                                          mask.get_single_value<float>(),
                                          settings);
      if (get_result("Image").should_compute()) {
        get_result("Image").allocate_single_value();
        get_result("Image").set_single_value(Color(float4(source.xyz() + light.xyz(), source.w)));
      }
      if (get_result("Lighting").should_compute()) {
        get_result("Lighting").allocate_single_value();
        get_result("Lighting").set_single_value(Color(light));
      }
      return;
    }

    Result &output = get_result("Image");
    Result &lighting = get_result("Lighting");
    const bool needs_image = output.should_compute();
    const bool needs_lighting = lighting.should_compute();
    if (!needs_image && !needs_lighting) {
      return;
    }
    const Domain domain = compute_domain();
    if (needs_image) {
      output.allocate_texture(domain);
    }
    if (needs_lighting) {
      lighting.allocate_texture(domain);
    }
    if (context().use_gpu()) {
      const char *shader_name = needs_image ? (needs_lighting ? "compositor_relight_both" :
                                                                "compositor_relight_image") :
                                              "compositor_relight_lighting";
      gpu::Shader *shader = context().get_shader(shader_name);
      GPU_shader_bind(shader);
      GPU_shader_uniform_2iv(shader, "output_size", domain.data_size);
      GPU_shader_uniform_1i(shader, "light_type", int(settings.type));
      GPU_shader_uniform_3fv(shader, "light_color", settings.color);
      GPU_shader_uniform_1f(shader, "strength", settings.strength);
      GPU_shader_uniform_3fv(shader, "light_direction", settings.direction);
      GPU_shader_uniform_3fv(shader, "light_position", settings.position);
      GPU_shader_uniform_1b(shader, "distance_falloff", settings.distance_falloff);
      GPU_shader_uniform_1f(shader, "minimum_distance", settings.minimum_distance);
      gpu::Texture *image_texture = image.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *normal_texture = normal.bind_as_texture_or_single_value(shader, "normal_tx");
      gpu::Texture *position_texture = position.bind_as_texture_or_single_value(shader,
                                                                                "position_tx");
      gpu::Texture *diffuse_texture = diffuse.bind_as_texture_or_single_value(shader,
                                                                              "diffuse_tx");
      gpu::Texture *mask_texture = mask.bind_as_texture_or_single_value(shader, "mask_tx");
      if (needs_image) {
        output.bind_as_image(shader, "output_img");
      }
      if (needs_lighting) {
        lighting.bind_as_image(shader, "lighting_img");
      }
      compute_dispatch_threads_at_least(shader, domain.data_size);
      GPU_shader_unbind();
      image.unbind_as_texture_or_single_value(image_texture);
      normal.unbind_as_texture_or_single_value(normal_texture);
      position.unbind_as_texture_or_single_value(position_texture);
      diffuse.unbind_as_texture_or_single_value(diffuse_texture);
      mask.unbind_as_texture_or_single_value(mask_texture);
      if (needs_image) {
        output.unbind_as_image();
      }
      if (needs_lighting) {
        lighting.unbind_as_image();
      }
    }
    else {
      parallel_for(domain.data_size, [&](const int2 texel) {
        const float4 source(image.load_pixel<Color, true>(texel));
        const float4 light = lighting_pixel(source,
                                            normal.load_pixel<float3, true>(texel),
                                            position.load_pixel<float3, true>(texel),
                                            float4(diffuse.load_pixel<Color, true>(texel)),
                                            mask.load_pixel<float, true>(texel),
                                            settings);
        if (needs_image) {
          output.store_pixel(texel, Color(float4(source.xyz() + light.xyz(), source.w)));
        }
        if (needs_lighting) {
          lighting.store_pixel(texel, Color(light));
        }
      });
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new RelightOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeRelight"_ustr);
  ntype.ui_name = "Relight";
  ntype.ui_description =
      "Add diffuse directional or point lighting using world-space Normal and Position passes";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_relight_cc
