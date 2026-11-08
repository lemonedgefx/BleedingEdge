/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_flow_tools_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_flow_to_speed_both)

#include "gpu_shader_compositor_flow_tools.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, output_size))) {
    return;
  }
  float2 previous = texture_load(previous_tx, p).xy;
  float2 next = texture_load(next_tx, p).xy;
  float a = tools_finite_motion(previous) ?
                tools_confidence(texture_load(previous_confidence_tx, p).x) :
                0.0f;
  float b = tools_finite_motion(next) ? tools_confidence(texture_load(next_confidence_tx, p).x) :
                                        0.0f;
  bool valid_a = a > 0.0f && a >= confidence_threshold;
  bool valid_b = b > 0.0f && b >= confidence_threshold;
#ifdef COMPUTE_SPEED
  imageStore(
      speed_img, p, float4(valid_a ? previous : float2(0.0f), valid_b ? -next : float2(0.0f)));
#endif
#ifdef COMPUTE_CONFIDENCE
  imageStore(confidence_img, p, float4(min(valid_a ? a : 0.0f, valid_b ? b : 0.0f)));
#endif
}
