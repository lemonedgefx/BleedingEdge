/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_optical_flow_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_optical_flow_output_both)

#include "gpu_shader_compositor_optical_flow.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, output_size))) {
    return;
  }
  float4 f = texture_load(forward_tx, p);
  float2 motion = f.xy;
  float reliability = 0.0f;
  float influence = clamp(texture_load(mask_tx, p).x, 0.0f, 1.0f);
  float2 q = float2(p) + motion;
  if (influence > 0.0f && texture_load(first_tx, p).y > 1.0e-6f && flow_inside(q, output_size)) {
    float4 b = flow_sample(second_tx, q);
    if (b.y > 1.0e-6f) {
      float error = flow_luminance(texture_load(first_tx, p)) - flow_luminance(b);
      reliability = f.z * f.w * influence / (1.0f + 100.0f * error * error);
      if (consistency_check) {
        float4 reverse = flow_sample(backward_tx, q);
        float2 back = reverse.xy;
        float tolerance = 1.0f + 0.01f * (length(motion) + length(back));
        reliability *= reverse.z > 0.0001f ? 1.0f / (1.0f + dot(motion + back, motion + back) /
                                                                (tolerance * tolerance)) :
                                             0.0f;
      }
    }
    else {
      motion = float2(0.0f);
    }
  }
  else {
    motion = float2(0.0f);
  }
#ifdef COMPUTE_FLOW
  imageStore(flow_img, p, float4(motion.x, motion.y, 0.0f, 0.0f));
#endif
#ifdef COMPUTE_CONFIDENCE
  imageStore(confidence_img, p, float4(clamp(reliability, 0.0f, 1.0f)));
#endif
}
