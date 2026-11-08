/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_ibk_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_ibk_compute_matte)

#include "gpu_shader_compositor_ibk.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_img)))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float4 plate = texture_load(plate_tx, texel);
  float matte = ibk_matte(color, plate, screen_channel, screen_balance, plate_exposure);
  imageStore(output_img, texel, float4(matte));
}
