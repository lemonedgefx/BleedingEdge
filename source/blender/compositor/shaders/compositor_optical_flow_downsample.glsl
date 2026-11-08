/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_optical_flow_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_optical_flow_downsample)

#include "gpu_shader_compositor_optical_flow.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, imageSize(output_img)))) {
    return;
  }
  float4 sum = float4(0.0f);
  for (int y = -2; y <= 2; y++) {
    for (int x = -2; x <= 2; x++) {
      sum += texture_load(input_tx, p * 2 + int2(x, y)) *
             (flow_gaussian_weight(x) * flow_gaussian_weight(y));
    }
  }
  imageStore(output_img, p, sum / 256.0f);
}
