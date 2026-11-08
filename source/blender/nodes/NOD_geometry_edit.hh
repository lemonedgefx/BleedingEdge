/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "BLI_span.hh"

namespace blender {
struct Mesh;
struct NodeGeometryCapturedSelection;
namespace bke {
enum class AttrDomain : int8_t;
}
namespace nodes {
class GeoNodeExecParams;
namespace geometry_edit {

enum class CaptureValidation : int8_t { Strict, OrderedTopology, ElementID };
enum class CapturePivot : int8_t { Fixed, Median, Active };
constexpr int SELECT_BY_INDEX_MANUAL = 0;
constexpr int SELECT_BY_INDEX_CAPTURED = 1;

/** Exact snapshot for gesture safety; saved captures use capture_matches_mesh instead. */
struct MeshSignature {
  int verts_num = 0;
  int edges_num = 0;
  int faces_num = 0;
  uint64_t topology_hash = 0;
  uint64_t position_hash = 0;
  uint64_t position_set_hash = 0;
  uint64_t identity_hash = 0;
  CaptureValidation validation = CaptureValidation::Strict;

  bool operator==(const MeshSignature &other) const = default;
};

MeshSignature mesh_signature(const Mesh &mesh);
MeshSignature capture_signature(const NodeGeometryCapturedSelection &capture);
void capture_initialize(NodeGeometryCapturedSelection &capture,
                        const MeshSignature &signature,
                        bke::AttrDomain domain,
                        Span<int> indices,
                        Span<int> point_indices);
void capture_copy(NodeGeometryCapturedSelection &dst, const NodeGeometryCapturedSelection &src);
bool capture_matches_mesh(const MeshSignature &capture, const Mesh &mesh);
/** Shared evaluation for the unified selection node and legacy saved capture nodes. */
void capture_execute(GeoNodeExecParams &params, bool with_pivot = false);

}  // namespace geometry_edit
}  // namespace nodes
}  // namespace blender
