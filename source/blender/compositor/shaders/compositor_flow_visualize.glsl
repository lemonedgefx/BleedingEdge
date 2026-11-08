/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_flow_tools_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_flow_visualize)

#include "gpu_shader_compositor_flow_tools.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, output_size))) {
    return;
  }
  float2 flow = texture_load(flow_tx, p).xy;
  float3 color = float3(0.0f);
  if (tools_finite_motion(flow)) {
    float hue = atan(flow.y, flow.x) / 6.283185307179586f + 1.0f;
    float value = min(1.0f, length(flow) / parameter) *
                  tools_confidence(texture_load(confidence_tx, p).x);
    float3 wheel = abs(fract(float3(hue) + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
    color = clamp(wheel - 1.0f, 0.0f, 1.0f) * value;
  }
  imageStore(output_img, p, float4(color, 1.0f));
}
