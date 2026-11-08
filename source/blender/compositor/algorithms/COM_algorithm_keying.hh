/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_scene_types.h"

#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

struct KeyingMatteSettings {
  float black_level = 0.0f;
  float white_level = 1.0f;
  int edge_search_radius = 3;
  float edge_tolerance = 0.1f;
  int blur_size = 0;
  int dilate_size = 0;
  int feather_size = 0;
  int feather_falloff = PROP_SMOOTH;
};

/* Refine a raw coverage matte using edge-preserving levels, masks, blur, morphology, and feather.
 * Inputs must be realized on the raw matte's domain; masks may also be single values. A single
 * value matte requires single value masks. Either
 * output may be null. Edges are identified before postprocessing. */
void keying_matte(Context &context,
                  const Result &input_matte,
                  const Result &garbage_matte,
                  const Result &core_matte,
                  const KeyingMatteSettings &settings,
                  Result *output_matte,
                  Result *output_edges = nullptr);

}  // namespace blender::compositor
