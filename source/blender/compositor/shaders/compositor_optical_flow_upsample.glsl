/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_optical_flow_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_optical_flow_upsample)

#include "gpu_shader_compositor_optical_flow.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, imageSize(output_img)))) {
    return;
  }
  float4 value = flow_sample(input_tx, float2(p) * 0.5f);
  imageStore(output_img, p, float4(2.0f * value.x, 2.0f * value.y, 0.0f, 0.0f));
}
