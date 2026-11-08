/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>
#include <utility>

#include "BLI_array.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "GEO_mesh_edge_loop.hh"

namespace blender::geometry {

namespace {

class EdgeLoopWalker {
 private:
  const Span<int2> edges_;
  const OffsetIndices<int> faces_;
  const Span<int> corner_verts_;
  const Span<int> corner_edges_;
  const Span<int> corner_to_face_;
  const GroupedSpan<int> vert_to_corners_;
  const GroupedSpan<int> vert_to_faces_;

  Array<int> edge_to_corner_offsets_;
  Array<int> edge_to_corner_indices_;
  GroupedSpan<int> edge_to_corners_;

  Array<int> vert_to_edge_offsets_;
  Array<int> vert_to_edge_indices_;
  GroupedSpan<int> vert_to_edges_;

  Array<int> nonwire_edge_count_;
  const VArray<bool> uv_seams_;
  const VArray<bool> sharp_edges_;
  const EdgeLoopDelimit delimit_;

  Array<bool> result_;
  Array<bool> wire_visited_;
  Array<int> visit_stamps_;
  int visit_stamp_ = 0;

 public:
  EdgeLoopWalker(const Mesh &mesh, const EdgeLoopDelimit delimit)
      : edges_(mesh.edges()),
        faces_(mesh.faces()),
        corner_verts_(mesh.corner_verts()),
        corner_edges_(mesh.corner_edges()),
        corner_to_face_(mesh.corner_to_face_map()),
        vert_to_corners_(mesh.vert_to_corner_map()),
        vert_to_faces_(mesh.vert_to_face_map()),
        nonwire_edge_count_(mesh.verts_num, 0),
        uv_seams_(
            *mesh.attributes().lookup_or_default<bool>("uv_seam", bke::AttrDomain::Edge, false)),
        sharp_edges_(*mesh.attributes().lookup_or_default<bool>(
            "sharp_edge", bke::AttrDomain::Edge, false)),
        delimit_(delimit),
        result_(mesh.edges_num, false),
        wire_visited_(mesh.edges_num, false),
        visit_stamps_(mesh.edges_num, 0)
  {
    edge_to_corners_ = bke::mesh::build_edge_to_corner_map(
        corner_edges_, mesh.edges_num, edge_to_corner_offsets_, edge_to_corner_indices_);
    vert_to_edges_ = bke::mesh::build_vert_to_edge_map(
        edges_, mesh.verts_num, vert_to_edge_offsets_, vert_to_edge_indices_);

    for (const int edge : edges_.index_range()) {
      if (this->edge_is_wire(edge)) {
        continue;
      }
      const int2 verts = edges_[edge];
      nonwire_edge_count_[verts[0]]++;
      if (verts[1] != verts[0]) {
        nonwire_edge_count_[verts[1]]++;
      }
    }
  }

  Array<bool> select(const IndexMask &seed_edges)
  {
    if (seed_edges.size() == edges_.size()) {
      result_.fill(true);
      return std::move(result_);
    }

    seed_edges.foreach_index([&](const int edge) {
      if (!edges_.index_range().contains(edge)) {
        return;
      }
      result_[edge] = true;
      const int face_count = this->edge_face_count(edge);
      if (face_count == 0) {
        this->walk_wire_component(edge);
      }
      else if (face_count > 2) {
        this->walk_nonmanifold_loop(edge, face_count);
      }
      else {
        this->walk_standard_loop(edge);
      }
    });
    return std::move(result_);
  }

 private:
  int edge_face_count(const int edge) const
  {
    return int(edge_to_corners_[edge].size());
  }

  bool edge_is_wire(const int edge) const
  {
    return this->edge_face_count(edge) == 0;
  }

  bool edge_is_boundary(const int edge) const
  {
    return this->edge_face_count(edge) == 1;
  }

  bool edge_is_manifold(const int edge) const
  {
    return this->edge_face_count(edge) == 2;
  }

  int other_vert(const int edge, const int vert) const
  {
    const int2 verts = edges_[edge];
    if (verts[0] == vert) {
      return verts[1];
    }
    if (verts[1] == vert) {
      return verts[0];
    }
    return -1;
  }

  int face_corner_prev(const int corner) const
  {
    const int face = corner_to_face_[corner];
    return bke::mesh::face_corner_prev(faces_[face], corner);
  }

  int face_corner_next(const int corner) const
  {
    const int face = corner_to_face_[corner];
    return bke::mesh::face_corner_next(faces_[face], corner);
  }

  /** Return the other corner edge in the same face that touches `vert`. */
  int adjacent_corner_at_vert(const int corner, const int vert) const
  {
    if (corner_verts_[corner] == vert) {
      return this->face_corner_prev(corner);
    }
    const int next_corner = this->face_corner_next(corner);
    if (corner_verts_[next_corner] == vert) {
      return next_corner;
    }
    return -1;
  }

  int other_manifold_corner(const int edge, const int corner) const
  {
    const Span<int> corners = edge_to_corners_[edge];
    if (corners.size() != 2) {
      return -1;
    }
    if (corners[0] == corner) {
      return corners[1];
    }
    if (corners[1] == corner) {
      return corners[0];
    }
    return -1;
  }

  bool edge_is_single_ngon_boundary(const int edge) const
  {
    const Span<int> corners = edge_to_corners_[edge];
    if (corners.size() != 1) {
      return false;
    }
    const int corner = corners.first();
    const int face = corner_to_face_[corner];
    if (faces_[face].size() <= 4) {
      return false;
    }
    const int prev_edge = corner_edges_[this->face_corner_prev(corner)];
    const int next_edge = corner_edges_[this->face_corner_next(corner)];
    return this->edge_is_boundary(prev_edge) || this->edge_is_boundary(next_edge);
  }

  bool delimit_by_mark(const int current_edge,
                       const int next_edge,
                       const int vert,
                       const VArray<bool> &marks) const
  {
    /* Match the Edit Mode walker: a walk starting on a mark remains on marked edges. A walk on
     * unmarked edges stops at a vertex incident to any marked, non-wire edge. */
    if (marks[current_edge]) {
      return !marks[next_edge] && !this->edge_is_wire(next_edge);
    }
    for (const int edge : vert_to_edges_[vert]) {
      if (!this->edge_is_wire(edge) && marks[edge]) {
        return true;
      }
    }
    return false;
  }

  bool delimit_by_edge_mark(const int current_edge, const int next_edge, const int vert) const
  {
    if (flag_is_set(delimit_, EdgeLoopDelimit::Seam) &&
        this->delimit_by_mark(current_edge, next_edge, vert, uv_seams_))
    {
      return true;
    }
    if (flag_is_set(delimit_, EdgeLoopDelimit::Sharp) &&
        this->delimit_by_mark(current_edge, next_edge, vert, sharp_edges_))
    {
      return true;
    }
    return false;
  }

  int find_hub_face(const int edge) const
  {
    if (this->edge_is_boundary(edge)) {
      return -1;
    }
    const int2 verts = edges_[edge];
    const bool has_three_connected_faces = (nonwire_edge_count_[verts[0]] == 3 &&
                                            vert_to_faces_[verts[0]].size() == 3) ||
                                           (nonwire_edge_count_[verts[1]] == 3 &&
                                            vert_to_faces_[verts[1]].size() == 3);
    if (!has_three_connected_faces) {
      return -1;
    }

    int best_face = -1;
    int best_size = 0;
    for (const int corner : edge_to_corners_[edge]) {
      const int face = corner_to_face_[corner];
      if (faces_[face].size() > best_size) {
        best_face = face;
        best_size = int(faces_[face].size());
      }
    }
    return best_size > 4 ? best_face : -1;
  }

  int find_hub_next_edge(const int current_edge, const int vert, const int hub_face) const
  {
    if (nonwire_edge_count_[vert] != 3) {
      return -1;
    }

    int candidate = -1;
    for (const int corner : faces_[hub_face]) {
      if (corner_edges_[corner] != current_edge) {
        continue;
      }
      const int adjacent_corner = this->adjacent_corner_at_vert(corner, vert);
      if (adjacent_corner == -1) {
        continue;
      }
      const int edge = corner_edges_[adjacent_corner];
      if (this->edge_is_boundary(edge)) {
        continue;
      }
      if (candidate != -1 && candidate != edge) {
        return -1;
      }
      candidate = edge;
    }
    return candidate;
  }

  int find_interior_next_edge(const int current_edge, const int vert) const
  {
    const int edge_count = nonwire_edge_count_[vert];
    if (edge_count != 2 && edge_count != 4) {
      return -1;
    }

    const Span<int> current_corners = edge_to_corners_[current_edge];
    if (current_corners.is_empty()) {
      return -1;
    }
    int corner = current_corners.first();
    const int crossings = edge_count / 2;
    for ([[maybe_unused]] const int i : IndexRange(crossings)) {
      const int adjacent_corner = this->adjacent_corner_at_vert(corner, vert);
      if (adjacent_corner == -1) {
        return -1;
      }
      const int edge = corner_edges_[adjacent_corner];
      if (!this->edge_is_manifold(edge)) {
        return -1;
      }
      corner = this->other_manifold_corner(edge, adjacent_corner);
      if (corner == -1) {
        return -1;
      }
    }
    return corner_edges_[corner];
  }

  int find_boundary_next_edge(const int current_edge, const int vert) const
  {
    if (flag_is_set(delimit_, EdgeLoopDelimit::InnerCorner) && nonwire_edge_count_[vert] > 3) {
      return -1;
    }
    if (flag_is_set(delimit_, EdgeLoopDelimit::OuterCorner) && nonwire_edge_count_[vert] == 2 &&
        !this->edge_is_single_ngon_boundary(current_edge))
    {
      return -1;
    }

    const Span<int> current_corners = edge_to_corners_[current_edge];
    if (current_corners.size() != 1) {
      return -1;
    }
    int corner = current_corners.first();
    const int max_steps = int(vert_to_corners_[vert].size()) + 1;
    for ([[maybe_unused]] const int step : IndexRange(max_steps)) {
      const int adjacent_corner = this->adjacent_corner_at_vert(corner, vert);
      if (adjacent_corner == -1) {
        return -1;
      }
      const int edge = corner_edges_[adjacent_corner];
      if (this->edge_is_manifold(edge)) {
        corner = this->other_manifold_corner(edge, adjacent_corner);
        if (corner == -1) {
          return -1;
        }
        continue;
      }
      return this->edge_is_boundary(edge) ? edge : -1;
    }
    return -1;
  }

  int find_standard_next_edge(const int current_edge,
                              const int vert,
                              const bool is_boundary_loop,
                              const bool start_is_single,
                              const int hub_face) const
  {
    if (hub_face != -1) {
      return this->find_hub_next_edge(current_edge, vert, hub_face);
    }

    const int next_edge = is_boundary_loop ? this->find_boundary_next_edge(current_edge, vert) :
                                             this->find_interior_next_edge(current_edge, vert);
    if (next_edge == -1 || this->delimit_by_edge_mark(current_edge, next_edge, vert)) {
      return -1;
    }
    if (is_boundary_loop && flag_is_set(delimit_, EdgeLoopDelimit::Ngon) &&
        start_is_single != this->edge_is_single_ngon_boundary(next_edge))
    {
      return -1;
    }
    return next_edge;
  }

  int next_visit_stamp()
  {
    if (visit_stamp_ == std::numeric_limits<int>::max()) {
      visit_stamps_.fill(0);
      visit_stamp_ = 1;
    }
    else {
      visit_stamp_++;
    }
    return visit_stamp_;
  }

  void walk_standard_direction(const int seed_edge,
                               const int start_last_vert,
                               const bool is_boundary_loop,
                               const bool start_is_single,
                               const int hub_face,
                               const int visit_stamp)
  {
    int current_edge = seed_edge;
    int last_vert = start_last_vert;
    while (true) {
      const int vert = this->other_vert(current_edge, last_vert);
      if (vert == -1) {
        return;
      }
      const int next_edge = this->find_standard_next_edge(
          current_edge, vert, is_boundary_loop, start_is_single, hub_face);
      if (next_edge == -1 || visit_stamps_[next_edge] == visit_stamp) {
        return;
      }
      visit_stamps_[next_edge] = visit_stamp;
      result_[next_edge] = true;
      current_edge = next_edge;
      last_vert = vert;
    }
  }

  void walk_standard_loop(const int seed_edge)
  {
    const int visit_stamp = this->next_visit_stamp();
    visit_stamps_[seed_edge] = visit_stamp;

    const bool is_boundary_loop = this->edge_is_boundary(seed_edge);
    const bool start_is_single = is_boundary_loop && this->edge_is_single_ngon_boundary(seed_edge);
    const int hub_face = is_boundary_loop ? -1 : this->find_hub_face(seed_edge);
    const int2 verts = edges_[seed_edge];
    this->walk_standard_direction(
        seed_edge, verts[0], is_boundary_loop, start_is_single, hub_face, visit_stamp);
    this->walk_standard_direction(
        seed_edge, verts[1], is_boundary_loop, start_is_single, hub_face, visit_stamp);
  }

  int find_nonmanifold_next_edge(const int current_edge,
                                 const int vert,
                                 const int face_count,
                                 const int visit_stamp) const
  {
    int candidate = -1;
    const int max_steps = int(vert_to_corners_[vert].size()) + 1;
    for (const int current_corner : edge_to_corners_[current_edge]) {
      int corner = current_corner;
      for ([[maybe_unused]] const int step : IndexRange(max_steps)) {
        const int adjacent_corner = this->adjacent_corner_at_vert(corner, vert);
        if (adjacent_corner == -1) {
          break;
        }
        const int edge = corner_edges_[adjacent_corner];
        const int adjacent_face_count = this->edge_face_count(edge);
        if (adjacent_face_count == 2) {
          corner = this->other_manifold_corner(edge, adjacent_corner);
          if (corner == -1) {
            break;
          }
          continue;
        }
        if (adjacent_face_count == face_count && visit_stamps_[edge] != visit_stamp) {
          if (candidate != -1 && candidate != edge) {
            /* More than one continuation is a junction, so there is no correct loop direction. */
            return -1;
          }
          candidate = edge;
        }
        break;
      }
    }
    return candidate;
  }

  void walk_nonmanifold_direction(const int seed_edge,
                                  const int start_last_vert,
                                  const int face_count,
                                  const int visit_stamp)
  {
    int current_edge = seed_edge;
    int last_vert = start_last_vert;
    while (true) {
      const int vert = this->other_vert(current_edge, last_vert);
      if (vert == -1) {
        return;
      }
      const int next_edge = this->find_nonmanifold_next_edge(
          current_edge, vert, face_count, visit_stamp);
      if (next_edge == -1 || visit_stamps_[next_edge] == visit_stamp) {
        return;
      }
      visit_stamps_[next_edge] = visit_stamp;
      result_[next_edge] = true;
      current_edge = next_edge;
      last_vert = vert;
    }
  }

  void walk_nonmanifold_loop(const int seed_edge, const int face_count)
  {
    const int visit_stamp = this->next_visit_stamp();
    visit_stamps_[seed_edge] = visit_stamp;
    const int2 verts = edges_[seed_edge];
    this->walk_nonmanifold_direction(seed_edge, verts[0], face_count, visit_stamp);
    this->walk_nonmanifold_direction(seed_edge, verts[1], face_count, visit_stamp);
  }

  void walk_wire_component(const int seed_edge)
  {
    if (wire_visited_[seed_edge]) {
      return;
    }
    Vector<int> stack;
    stack.append(seed_edge);
    wire_visited_[seed_edge] = true;
    while (!stack.is_empty()) {
      const int edge = stack.pop_last();
      result_[edge] = true;
      const int2 verts = edges_[edge];
      for (const int vert : {verts[0], verts[1]}) {
        for (const int connected_edge : vert_to_edges_[vert]) {
          if (!wire_visited_[connected_edge] && this->edge_is_wire(connected_edge)) {
            wire_visited_[connected_edge] = true;
            stack.append(connected_edge);
          }
        }
      }
    }
  }
};

class FaceLoopWalker {
 private:
  const OffsetIndices<int> faces_;
  const Span<int> corner_edges_;
  const Array<int> corner_to_face_;
  const FaceLoopDirection direction_;

  Array<int> edge_to_corner_offsets_;
  Array<int> edge_to_corner_indices_;
  GroupedSpan<int> edge_to_corners_;

  Array<bool> result_;
  Array<int> visit_stamps_;
  int visit_stamp_ = 0;

 public:
  FaceLoopWalker(const Mesh &mesh, const FaceLoopDirection direction)
      : faces_(mesh.faces()),
        corner_edges_(mesh.corner_edges()),
        corner_to_face_(mesh.corner_to_face_map()),
        direction_(direction),
        result_(mesh.faces_num, false),
        visit_stamps_(mesh.faces_num, 0)
  {
    edge_to_corners_ = bke::mesh::build_edge_to_corner_map(
        corner_edges_, mesh.edges_num, edge_to_corner_offsets_, edge_to_corner_indices_);
  }

  Array<bool> select(const IndexMask &seed_faces)
  {
    if (seed_faces.size() == faces_.size()) {
      result_.fill(true);
      return std::move(result_);
    }

    seed_faces.foreach_index([&](const int seed_face) {
      if (!faces_.index_range().contains(seed_face)) {
        return;
      }
      result_[seed_face] = true;
      const IndexRange face = faces_[seed_face];
      if (face.size() != 4) {
        return;
      }

      const int visit_stamp = this->next_visit_stamp();
      visit_stamps_[seed_face] = visit_stamp;
      const int first_corner = direction_ == FaceLoopDirection::U ? 1 : 0;
      this->walk_direction(seed_face, corner_edges_[face[first_corner]], visit_stamp);
      this->walk_direction(seed_face, corner_edges_[face[first_corner + 2]], visit_stamp);
    });
    return std::move(result_);
  }

 private:
  int next_visit_stamp()
  {
    if (visit_stamp_ == std::numeric_limits<int>::max()) {
      visit_stamps_.fill(0);
      visit_stamp_ = 0;
    }
    return ++visit_stamp_;
  }

  int other_face_corner(const int edge, const int current_face) const
  {
    const Span<int> corners = edge_to_corners_[edge];
    if (corners.size() != 2) {
      return -1;
    }
    if (corner_to_face_[corners[0]] == current_face && corner_to_face_[corners[1]] != current_face)
    {
      return corners[1];
    }
    if (corner_to_face_[corners[1]] == current_face && corner_to_face_[corners[0]] != current_face)
    {
      return corners[0];
    }
    return -1;
  }

  void walk_direction(const int seed_face, const int seed_edge, const int visit_stamp)
  {
    int current_face = seed_face;
    int current_edge = seed_edge;
    while (true) {
      const int next_corner = this->other_face_corner(current_edge, current_face);
      if (next_corner == -1) {
        return;
      }
      const int next_face = corner_to_face_[next_corner];
      if (visit_stamps_[next_face] == visit_stamp) {
        return;
      }
      const IndexRange face = faces_[next_face];
      if (face.size() != 4) {
        return;
      }

      visit_stamps_[next_face] = visit_stamp;
      result_[next_face] = true;
      const int corner_in_face = next_corner - face.start();
      current_edge = corner_edges_[face[(corner_in_face + 2) % 4]];
      current_face = next_face;
    }
  }
};

}  // namespace

Array<bool> edge_loop_selection(const Mesh &mesh,
                                const IndexMask &seed_edges,
                                const EdgeLoopDelimit delimit)
{
  return EdgeLoopWalker(mesh, delimit).select(seed_edges);
}

Array<bool> face_loop_selection(const Mesh &mesh,
                                const IndexMask &seed_faces,
                                const FaceLoopDirection direction)
{
  return FaceLoopWalker(mesh, direction).select(seed_faces);
}

}  // namespace blender::geometry
