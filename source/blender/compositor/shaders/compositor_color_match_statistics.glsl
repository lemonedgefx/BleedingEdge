/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_color_match_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_color_match_statistics)

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_img)))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float mask = texture_load(mask_tx, texel).x;
  if (!(color.w > 1.0e-6f) || isnan(color.w) || isinf(color.w) || isnan(mask) || isinf(mask) ||
      mask <= 0.0f)
  {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }
  float3 value = color.xyz / color.w;
  if (!all(lessThan(abs(value), float3(1.0e10f)))) {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }
  if (luminance_only) {
    value = float3(dot(value, luminance_coefficients));
  }
  float weight = clamp(mask, 0.0f, 1.0f) * min(color.w, 1.0f);
  if (squared_difference) {
    value -= mean;
    value *= value;
  }
  imageStore(output_img, texel, float4(value * weight, weight));
}
