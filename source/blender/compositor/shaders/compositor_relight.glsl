/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_relight_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_relight_both)

#include "gpu_shader_compositor_texture_utilities.glsl"

bool finite_vector(float3 value)
{
  return !any(isnan(value)) && !any(isinf(value));
}

float3 compute_lighting(float4 image, float3 normal, float3 position, float4 diffuse, float mask)
{
  if (!(image.w > 1.0e-6f) || !(mask > 0.0f) || !finite_vector(normal)) {
    return float3(0.0f);
  }
  float normal_length = length(normal);
  if (!(normal_length > 1.0e-6f) || isinf(normal_length)) {
    return float3(0.0f);
  }
  float3 direction = light_direction;
  float attenuation = 1.0f;
  if (light_type == 1) {
    if (!finite_vector(position)) {
      return float3(0.0f);
    }
    direction = light_position - position;
    float distance_squared = dot(direction, direction);
    if (distance_falloff) {
      attenuation = 1.0f / max(distance_squared, minimum_distance * minimum_distance);
    }
  }
  float direction_length = length(direction);
  if (!(direction_length > 1.0e-6f) || isinf(direction_length)) {
    return float3(0.0f);
  }
  float lambert = max(0.0f, dot(normal / normal_length, direction / direction_length));
  float3 albedo = diffuse.w > 1.0e-6f ? diffuse.xyz / diffuse.w : float3(0.0f);
  float influence = strength * lambert * attenuation * clamp(mask, 0.0f, 1.0f) * image.w;
  return albedo * light_color * influence;
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, output_size))) {
    return;
  }
  float4 source = texture_load(input_tx, texel);
  float3 light = compute_lighting(source,
                                  texture_load(normal_tx, texel).xyz,
                                  texture_load(position_tx, texel).xyz,
                                  texture_load(diffuse_tx, texel),
                                  texture_load(mask_tx, texel).x);
#ifdef COMPUTE_IMAGE
  imageStore(output_img, texel, float4(source.xyz + light, source.w));
#endif
#ifdef COMPUTE_LIGHTING
  imageStore(lighting_img, texel, float4(light, source.w));
#endif
}
