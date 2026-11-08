/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_flow_tools_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_flow_interpolate_both)

#include "gpu_shader_compositor_flow_tools.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, output_size))) {
    return;
  }
  float3 a = tools_inverse_warp(
      forward_tx, backward_tx, forward_confidence_tx, float2(p), parameter, output_size);
  float3 b = tools_inverse_warp(
      backward_tx, forward_tx, backward_confidence_tx, float2(p), 1.0f - parameter, output_size);
  float4 ca = tools_sample(first_tx, a.xy);
  float4 cb = tools_sample(second_tx, b.xy);
  bool supported_a = ca.w > 1.0e-6f && !any(isnan(ca)) && !any(isinf(ca));
  bool supported_b = cb.w > 1.0e-6f && !any(isnan(cb)) && !any(isinf(cb));
  float wa = supported_a && a.z >= confidence_threshold ? (1.0f - parameter) * a.z : 0.0f;
  float wb = supported_b && b.z >= confidence_threshold ? parameter * b.z : 0.0f;
  float total = wa + wb;
#ifdef COMPUTE_IMAGE
  float4 color = float4(0.0f);
  if (total > 1.0e-6f) {
    color = ((wa > 0.0f ? ca * wa : float4(0.0f)) + (wb > 0.0f ? cb * wb : float4(0.0f))) / total;
  }
  else if (fallback_mode == 0) {
    color = mix(texture_load(first_tx, p), texture_load(second_tx, p), parameter);
  }
  else if (fallback_mode == 1) {
    color = parameter < 0.5f ? texture_load(first_tx, p) : texture_load(second_tx, p);
  }
  imageStore(output_img, p, color);
#endif
#ifdef COMPUTE_CONFIDENCE
  imageStore(confidence_img, p, float4(tools_confidence(total)));
#endif
}
