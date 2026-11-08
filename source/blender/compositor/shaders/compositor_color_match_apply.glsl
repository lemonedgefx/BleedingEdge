/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_color_match_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_color_match_apply)

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_img)))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float factor = clamp(texture_load(factor_tx, texel).x, 0.0f, 1.0f);
  float3 corrected = color.xyz * gain + offset * color.w;
  imageStore(output_img, texel, float4(mix(color.xyz, corrected, factor), color.w));
}
