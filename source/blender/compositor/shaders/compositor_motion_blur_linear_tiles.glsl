/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur_linear_tiles)

#include "gpu_shader_compositor_texture_utilities.glsl"

shared float4 tile_motion[1024];

float2 maximum_motion(float2 a, float2 b)
{
  /* Equal magnitudes choose the first pixel, matching the CPU reduction. */
  return dot(a, a) >= dot(b, b) ? a : b;
}

void main()
{
  uint index = gl_LocalInvocationIndex;
  int2 size = texture_size(input_tx);
  int2 origin = int2(gl_WorkGroupID.xy) * 32;
  for (uint i = index; i < 1024u; i += 256u) {
    int2 texel = min(origin + int2(int(i % 32u), int(i / 32u)), size - int2(1));
    tile_motion[i] = texture_load(input_tx, texel);
  }
  barrier();
  for (uint stride = 512u; stride > 0u; stride /= 2u) {
    for (uint i = index; i < stride; i += 256u) {
      float4 a = tile_motion[i];
      float4 b = tile_motion[i + stride];
      tile_motion[i] = float4(maximum_motion(a.xy, b.xy), maximum_motion(a.zw, b.zw));
    }
    barrier();
  }
  if (index == 0u) {
    imageStore(output_img, int2(gl_WorkGroupID.xy), tile_motion[0]);
  }
}
