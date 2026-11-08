/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"

namespace blender {

struct Mesh;

namespace geometry {

/**
 * Dissolve the selected elements, merging the surrounding geometry instead of leaving holes,
 * matching the edit mode dissolve operators with their default options:
 *
 * - #AttrDomain::Point: Remove each selected vertex, merging the faces around it into one
 *   (like "Dissolve Vertices"). Vertices left between exactly two edges are removed and their
 *   edges are merged.
 * - #AttrDomain::Edge: Remove each selected edge, merging the faces on both sides (like
 *   "Dissolve Edges" with "Dissolve Vertices" and "Preserve Quads" enabled).
 * - #AttrDomain::Face: Merge each connected group of selected faces into a single face (like
 *   "Dissolve Faces").
 *
 * Only edges with exactly two distinct adjacent faces can be dissolved; other edges are ignored.
 * Regions whose merged boundary would not form a single loop (e.g. merging would create a face
 * with a hole) are left unchanged.
 *
 * \return The new mesh, or null if dissolving would not change the mesh.
 */
Mesh *dissolve_mesh_elements(const Mesh &mesh,
                             bke::AttrDomain domain,
                             const IndexMask &selection,
                             const bke::AttributeFilter &attribute_filter = {});

}  // namespace geometry
}  // namespace blender
