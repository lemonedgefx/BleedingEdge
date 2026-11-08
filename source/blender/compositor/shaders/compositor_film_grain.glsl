/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_film_grain_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_film_grain)

#include "gpu_shader_compositor_film_grain.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_img)))) {
    return;
  }
  float4 image = texture_load(input_tx, texel);
  float mask = clamp(texture_load(mask_tx, texel).x, 0.0f, 1.0f);
  if (image.w <= 1.0e-6f || mask <= 0.0f || amount == 0.0f) {
    imageStore(output_img, texel, image);
    return;
  }
  float luminance = max(0.0f, dot(image.xyz / image.w, luminance_coefficients));
  float t = luminance / (luminance + pivot);
  float3 weights = float3((1.0f - t) * (1.0f - t), 2.0f * t * (1.0f - t), t * t);
  float envelope = dot(weights, response);
  float2 position = float2(texel + data_offset) + float2(0.5f);
  uint random_seed = uint(seed);
  float monochrome = grain_noise(position, grain_size.x, random_seed);
  float3 noise = float3(monochrome);
  if (color_amount > 0.0f) {
    float3 colored = float3(monochrome,
                            grain_noise(position, grain_size.y, grain_hash(random_seed + 1U)),
                            grain_noise(position, grain_size.z, grain_hash(random_seed + 2U)));
    noise = mix(noise, colored, color_amount);
  }
  float scale = amount * envelope * mask * image.w;
  imageStore(output_img, texel, float4(image.xyz + noise * scale, image.w));
}
