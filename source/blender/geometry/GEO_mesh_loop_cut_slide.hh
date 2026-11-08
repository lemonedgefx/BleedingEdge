/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

struct Mesh;

namespace geometry {

struct LoopCutSlideResult {
  /** The changed mesh, or null when no valid edge ring was found. */
  Mesh *mesh = nullptr;
  /** Number of selected rings ignored because they were invalid or intersected another ring. */
  int skipped_rings = 0;
};

struct LoopCutSlidePreview {
  /** Consecutive pairs are the endpoints of the new loop edges, in mesh-local space. */
  Vector<float3> lines;
  int skipped_rings = 0;
};

/** Lightweight preview using exactly the ring acceptance, orientation and factor of the kernel.
 * Does not convert to BMesh, subdivide geometry, or change the input. Empty lines mean no cut. */
LoopCutSlidePreview preview_loop_cut_and_slide_mesh(const Mesh &mesh,
                                                    const IndexMask &seed_edges,
                                                    int cuts,
                                                    float factor);

/**
 * Insert evenly spaced edge loops through quad rings expanded from the selected seed edges, then
 * slide the new loops along the subdivided ring edges.
 *
 * The signed factor follows Edit Mode's clamped Edge Slide range. A factor of zero keeps the cuts
 * evenly spaced, -1 moves the first cut to one neighboring loop, and 1 moves the last cut to the
 * other. Ring orientation is propagated from the stored vertex order of each lowest-index seed.
 *
 * When `loop_index_attribute` is non-empty, an integer edge attribute is created. Every edge in a
 * newly created loop receives a unique zero-based loop index; all other edges receive -1.
 * When `loops_attribute` is non-empty, a boolean edge attribute selects every newly created loop.
 *
 * Intersecting edge rings are skipped because a single integer cannot identify both loops at the
 * crossing without changing the output contract.
 */
LoopCutSlideResult loop_cut_and_slide_mesh(const Mesh &mesh,
                                           const IndexMask &seed_edges,
                                           int cuts,
                                           float factor,
                                           StringRef loop_index_attribute = {},
                                           StringRef loops_attribute = {});

}  // namespace geometry
}  // namespace blender
