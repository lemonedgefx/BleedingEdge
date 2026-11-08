/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_optical_flow_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_optical_flow_prepare)

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, imageSize(output_img)))) {
    return;
  }
  float4 color = texture_load(input_tx, p);
  if (!(color.w > 1.0e-6f) || isnan(color.w) || isinf(color.w)) {
    imageStore(output_img, p, float4(0.0f));
    return;
  }
  float value = dot(color.xyz / color.w, luminance_coefficients);
  if (isnan(value) || isinf(value)) {
    imageStore(output_img, p, float4(0.0f));
    return;
  }
  float compressed = sign(value) * log(1.0f + abs(value));
  float alpha = min(color.w, 1.0f);
  imageStore(output_img, p, float4(compressed * alpha, alpha, 0.0f, 0.0f));
}
