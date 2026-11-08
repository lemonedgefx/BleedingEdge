/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "BLI_array.hh"
#include "BLI_enum_flags.hh"
#include "BLI_index_mask.hh"

namespace blender {

struct Mesh;

namespace geometry {

/** Options that stop edge-loop traversal at specific kinds of topology or edge marks. */
enum class EdgeLoopDelimit : uint8_t {
  None = 0,
  Seam = 1 << 0,
  Sharp = 1 << 1,
  Ngon = 1 << 2,
  InnerCorner = 1 << 3,
  OuterCorner = 1 << 4,
};
ENUM_OPERATORS(EdgeLoopDelimit)

/** The delimiter settings used by Edit Mode's edge-loop selection operator. */
inline constexpr EdgeLoopDelimit edge_loop_delimit_default = EdgeLoopDelimit::Ngon |
                                                             EdgeLoopDelimit::OuterCorner;

/** The two possible face-loop directions relative to the seed face's corner order. */
enum class FaceLoopDirection : uint8_t {
  /** Continue through the side edges (corners 1 and 3) of each seed quad. */
  U = 0,
  /** Continue through the bottom and top edges (corners 0 and 2) of each seed quad. */
  V = 1,
};

/**
 * Expand the selected seed edges into edge loops, following the same topology rules as Edit
 * Mode's "Multi Select Edge Loops" operator.
 *
 * The result is aligned with the mesh edge domain. Every seed edge is included. Traversal stops
 * at ambiguous junctions instead of choosing an arbitrary continuation.
 */
Array<bool> edge_loop_selection(const Mesh &mesh,
                                const IndexMask &seed_edges,
                                EdgeLoopDelimit delimit = edge_loop_delimit_default);

/**
 * Expand seed faces into quad face loops, matching Edit Mode face-loop traversal.
 *
 * `direction` chooses one of the two opposite edge pairs of every seed quad. The direction is
 * propagated through neighboring quads by always crossing the edge opposite the entering edge.
 * Every seed face is included. Traversal stops at boundaries, non-manifold edges, and non-quads.
 */
Array<bool> face_loop_selection(const Mesh &mesh,
                                const IndexMask &seed_faces,
                                FaceLoopDirection direction);

}  // namespace geometry
}  // namespace blender
