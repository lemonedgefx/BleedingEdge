/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_optical_flow_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_optical_flow_iterate)

#include "gpu_shader_compositor_optical_flow.glsl"

void main()
{
  int2 p = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, imageSize(output_img)))) {
    return;
  }
  imageStore(output_img,
             p,
             flow_iterate(gradient_tx, reference_tx, flow_tx, p, window_radius, max_motion));
}
