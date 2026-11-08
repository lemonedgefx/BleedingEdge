/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <atomic>

#include "BLI_array.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "GEO_mesh_dissolve.hh"
#include "GEO_randomize.hh"

namespace blender::geometry {

/**
 * The result of merging all faces of one region (faces connected by dissolved edges) into a
 * single face by walking around the region's boundary.
 */
struct MergedRegion {
  /** Source corner for each corner of the merged face (used for attribute propagation). */
  Vector<int> corners;
  /** Original edge following each corner in the merged face. */
  Vector<int> edges;
  /**
   * True if the region cannot be merged into a single valid face (e.g. the boundary consists of
   * multiple loops). Failed regions are left unchanged.
   */
  bool failed = false;
};

/**
 * A position on a region boundary: a directed half-edge. When `forward` is true, the half-edge
 * starts at `corner`'s vertex and traverses the corner's edge. When false (used for faces whose
 * winding is flipped relative to the walk), it starts at `corner`'s vertex and traverses the
 * previous corner's edge.
 */
struct WalkPos {
  int corner;
  bool forward;

  friend bool operator==(const WalkPos &a, const WalkPos &b)
  {
    return a.corner == b.corner && a.forward == b.forward;
  }
};

static int walk_pos_edge(const OffsetIndices<int> faces,
                         const Span<int> corner_edges,
                         const Span<int> corner_to_face_map,
                         const WalkPos pos)
{
  if (pos.forward) {
    return corner_edges[pos.corner];
  }
  const IndexRange face = faces[corner_to_face_map[pos.corner]];
  return corner_edges[bke::mesh::face_corner_prev(face, pos.corner)];
}

/**
 * Move to the next boundary half-edge: step to the next corner within the current face, then
 * while the edge at that position is dissolved, cross over it into the neighboring face,
 * adjusting the walk direction if that face's winding is flipped.
 */
static WalkPos walk_advance(const OffsetIndices<int> faces,
                            const Span<int> corner_verts,
                            const Span<int> corner_edges,
                            const GroupedSpan<int> edge_to_corner_map,
                            const Span<int> corner_to_face_map,
                            const Span<bool> dissolved,
                            const WalkPos pos)
{
  int face = corner_to_face_map[pos.corner];
  bool forward = pos.forward;
  int corner = forward ? bke::mesh::face_corner_next(faces[face], pos.corner) :
                         bke::mesh::face_corner_prev(faces[face], pos.corner);
  while (true) {
    const int owner = forward ? corner : bke::mesh::face_corner_prev(faces[face], corner);
    const int edge = corner_edges[owner];
    if (!dissolved[edge]) {
      return {corner, forward};
    }
    const Span<int> sides = edge_to_corner_map[edge];
    const int other = (sides[0] == owner) ? sides[1] : sides[0];
    const int other_face = corner_to_face_map[other];
    /* The vertex the walk is currently at; crossing the edge must continue from it. */
    const int vert = corner_verts[corner];
    if (corner_verts[other] == vert) {
      /* The neighboring face has flipped winding relative to the walk. */
      corner = other;
      forward = false;
    }
    else {
      corner = bke::mesh::face_corner_next(faces[other_face], other);
      forward = true;
      BLI_assert(corner_verts[corner] == vert);
    }
    face = other_face;
  }
}

static void walk_region_boundary(const OffsetIndices<int> faces,
                                 const Span<int> corner_verts,
                                 const Span<int> corner_edges,
                                 const GroupedSpan<int> edge_to_corner_map,
                                 const Span<int> corner_to_face_map,
                                 const Span<bool> dissolved,
                                 const Span<int> region_faces,
                                 MergedRegion &result)
{
  /* Find a boundary half-edge to start from and count the boundary size. */
  int boundary_num = 0;
  int start_corner = -1;
  for (const int face : region_faces) {
    for (const int corner : faces[face]) {
      if (!dissolved[corner_edges[corner]]) {
        boundary_num++;
        start_corner = corner;
      }
    }
  }
  if (boundary_num < 3) {
    result.failed = true;
    return;
  }

  result.corners.reserve(boundary_num);
  result.edges.reserve(boundary_num);

  const WalkPos start{start_corner, true};
  WalkPos pos = start;
  int steps = 0;
  do {
    result.corners.append(pos.corner);
    result.edges.append(walk_pos_edge(faces, corner_edges, corner_to_face_map, pos));
    pos = walk_advance(
        faces, corner_verts, corner_edges, edge_to_corner_map, corner_to_face_map, dissolved, pos);
    steps++;
  } while (!(pos == start) && steps < boundary_num);

  /* The walk must return to the start and visit the entire boundary in a single loop. Otherwise
   * the merged face would have a hole or consist of multiple loops, which a face can't represent.
   */
  if (!(pos == start) || steps != boundary_num) {
    result.failed = true;
    result.corners.clear_and_shrink();
    result.edges.clear_and_shrink();
  }
}

/** Gather vertex group data and generic attributes. */
static void gather_vert_attributes(const Mesh &src_mesh,
                                   const bke::AttributeFilter &attribute_filter,
                                   const IndexMask &vert_mask,
                                   Mesh &dst_mesh)
{
  Set<std::string> vertex_group_names;
  for (bDeformGroup &group : src_mesh.vertex_group_names) {
    vertex_group_names.add(group.name);
  }
  const Span<MDeformVert> src = src_mesh.deform_verts();
  if (!vertex_group_names.is_empty() && !src.is_empty()) {
    MutableSpan<MDeformVert> dst = dst_mesh.deform_verts_for_write();
    bke::gather_deform_verts(src, vert_mask, dst);
  }
  bke::gather_attributes(src_mesh.attributes(),
                         bke::AttrDomain::Point,
                         bke::AttrDomain::Point,
                         bke::attribute_filter_with_skip_ref(attribute_filter, vertex_group_names),
                         vert_mask,
                         dst_mesh.attributes_for_write());
}

Mesh *dissolve_mesh_elements(const Mesh &src_mesh,
                             const bke::AttrDomain domain,
                             const IndexMask &selection,
                             const bke::AttributeFilter &attribute_filter)
{
  BLI_assert(
      ELEM(domain, bke::AttrDomain::Point, bke::AttrDomain::Edge, bke::AttrDomain::Face));
  const int src_verts_num = src_mesh.verts_num;
  const Span<int2> src_edges = src_mesh.edges();
  const OffsetIndices<int> src_faces = src_mesh.faces();
  const Span<int> corner_verts = src_mesh.corner_verts();
  const Span<int> corner_edges = src_mesh.corner_edges();

  Array<int> edge_to_corner_offsets;
  Array<int> edge_to_corner_indices;
  const GroupedSpan<int> edge_to_corner_map = bke::mesh::build_edge_to_corner_map(
      corner_edges, src_edges.size(), edge_to_corner_offsets, edge_to_corner_indices);
  const Array<int> corner_to_face_map = src_mesh.corner_to_face_map();

  /* Only edges with exactly two distinct adjacent faces can be dissolved, matching the BMesh
   * operators which ignore boundary, wire and non-manifold edges. */
  const auto edge_faces = [&](const int edge, int &r_face_1, int &r_face_2) -> bool {
    const Span<int> corners = edge_to_corner_map[edge];
    if (corners.size() != 2) {
      return false;
    }
    r_face_1 = corner_to_face_map[corners[0]];
    r_face_2 = corner_to_face_map[corners[1]];
    return r_face_1 != r_face_2;
  };

  /* Which edges to remove by merging the faces on both sides, and which vertices are allowed to
   * dissolve if they end up between exactly two edges. Both depend on the selection domain. */
  Array<bool> dissolved(src_edges.size(), false);
  Array<bool> vert_candidate(src_verts_num, false);
  bool may_change = false;
  switch (domain) {
    case bke::AttrDomain::Edge: {
      /* Vertex candidates are determined later, from the edges that actually dissolve. */
      std::atomic<bool> any_dissolved = false;
      selection.foreach_index(
          [&](const int edge) {
            int face_1, face_2;
            if (edge_faces(edge, face_1, face_2)) {
              dissolved[edge] = true;
              any_dissolved.store(true, std::memory_order_relaxed);
            }
          },
          exec_mode::grain_size(2048));
      may_change = any_dissolved;
      break;
    }
    case bke::AttrDomain::Point: {
      /* Dissolve every edge around each selected vertex; the vertices themselves are candidates
       * so that boundary vertices (whose edges cannot dissolve) still collapse. */
      selection.foreach_index([&](const int vert) { vert_candidate[vert] = true; },
                              exec_mode::grain_size(4096));
      threading::parallel_for(src_edges.index_range(), 2048, [&](const IndexRange range) {
        for (const int edge : range) {
          const int2 verts = src_edges[edge];
          if (!vert_candidate[verts[0]] && !vert_candidate[verts[1]]) {
            continue;
          }
          int face_1, face_2;
          if (edge_faces(edge, face_1, face_2)) {
            dissolved[edge] = true;
          }
        }
      });
      may_change = !selection.is_empty();
      break;
    }
    case bke::AttrDomain::Face: {
      /* Dissolve every edge that only separates selected faces, merging each connected group of
       * selected faces into one. Vertices are kept, like the edit mode operator default. */
      Array<bool> face_selected(src_faces.size(), false);
      selection.foreach_index([&](const int face) { face_selected[face] = true; },
                              exec_mode::grain_size(4096));
      std::atomic<bool> any_dissolved = false;
      threading::parallel_for(src_edges.index_range(), 2048, [&](const IndexRange range) {
        for (const int edge : range) {
          int face_1, face_2;
          if (!edge_faces(edge, face_1, face_2)) {
            continue;
          }
          if (face_selected[face_1] && face_selected[face_2]) {
            dissolved[edge] = true;
            any_dissolved.store(true, std::memory_order_relaxed);
          }
        }
      });
      may_change = any_dissolved;
      break;
    }
    default:
      break;
  }
  if (!may_change) {
    return nullptr;
  }

  /* Group faces connected by dissolved edges into regions; each region merges into one face. */
  AtomicDisjointSet regions_set(src_faces.size());
  threading::parallel_for(src_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int edge : range) {
      if (!dissolved[edge]) {
        continue;
      }
      const Span<int> corners = edge_to_corner_map[edge];
      regions_set.join(corner_to_face_map[corners[0]], corner_to_face_map[corners[1]]);
    }
  });
  Array<int> face_region(src_faces.size());
  const int regions_num = regions_set.calc_reduced_ids(face_region);

  /* Group face indices by region with a counting sort. Because faces are visited in order, each
   * region's face list is sorted, and its first face is the region's minimum face index. */
  Array<int> region_offset_data(regions_num + 1, 0);
  for (const int face : src_faces.index_range()) {
    region_offset_data[face_region[face]]++;
  }
  const OffsetIndices<int> region_offsets = offset_indices::accumulate_counts_to_offsets(
      region_offset_data);
  Array<int> region_faces(src_faces.size());
  {
    Array<int> cursor(regions_num, 0);
    for (const int face : src_faces.index_range()) {
      const int region = face_region[face];
      region_faces[region_offsets[region].start() + cursor[region]++] = face;
    }
  }

  /* Walk the boundary of every multi-face region to build the merged faces. Failed regions keep
   * their original faces; their edges are marked as not dissolved. */
  Array<MergedRegion> merged_regions(regions_num);
  threading::parallel_for(IndexRange(regions_num), 16, [&](const IndexRange range) {
    for (const int region : range) {
      const Span<int> faces_of_region = region_faces.as_span().slice(region_offsets[region]);
      if (faces_of_region.size() < 2) {
        continue;
      }
      MergedRegion &merged = merged_regions[region];
      walk_region_boundary(src_faces,
                           corner_verts,
                           corner_edges,
                           edge_to_corner_map,
                           corner_to_face_map,
                           dissolved,
                           faces_of_region,
                           merged);
      if (merged.failed) {
        for (const int face : faces_of_region) {
          for (const int corner : src_faces[face]) {
            dissolved[corner_edges[corner]] = false;
          }
        }
      }
    }
  });

  if (domain == bke::AttrDomain::Edge) {
    /* Dissolve vertices that are left between exactly two edges, matching the edit mode operator
     * defaults (`use_verts` on, angle threshold of 180° and "Preserve Quads" enabled: the verts
     * of an edge between two triangles are kept, so dissolving such an edge gives a quad). */
    selection.foreach_index(
        [&](const int edge) {
          if (!dissolved[edge]) {
            return;
          }
          const Span<int> corners = edge_to_corner_map[edge];
          const int face_1 = corner_to_face_map[corners[0]];
          const int face_2 = corner_to_face_map[corners[1]];
          if (src_faces[face_1].size() == 3 && src_faces[face_2].size() == 3) {
            return;
          }
          vert_candidate[src_edges[edge][0]] = true;
          vert_candidate[src_edges[edge][1]] = true;
        },
        exec_mode::grain_size(2048));
  }

  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      src_edges, src_verts_num, vert_to_edge_offsets, vert_to_edge_indices);

  /* Reduce candidates to vertices with exactly two remaining edges and remember those edges. */
  Array<int2> candidate_edges(src_verts_num);
  threading::parallel_for(IndexRange(src_verts_num), 2048, [&](const IndexRange range) {
    for (const int vert : range) {
      if (!vert_candidate[vert]) {
        continue;
      }
      int num = 0;
      int2 kept_edges(-1, -1);
      for (const int edge : vert_to_edge_map[vert]) {
        if (dissolved[edge]) {
          continue;
        }
        if (num < 2) {
          kept_edges[num] = edge;
        }
        num++;
      }
      if (num != 2) {
        vert_candidate[vert] = false;
        continue;
      }
      candidate_edges[vert] = kept_edges;
    }
  });

  /* Merge chains of edges connected by dissolving vertices into single reused edges. Chains are
   * expected to be rare and short, so this pass is serial. */
  Array<bool> vert_dissolved(src_verts_num, false);
  Array<bool> edge_kept(src_edges.size());
  threading::parallel_for(src_edges.index_range(), 4096, [&](const IndexRange range) {
    for (const int edge : range) {
      edge_kept[edge] = !dissolved[edge];
    }
  });
  /* Maps edges merged into a chain to the edge reused for the whole chain. */
  Map<int, int> chain_edge_map;
  /* New endpoints for edges reused by a chain. */
  Map<int, int2> reused_edge_verts;
  {
    Array<bool> visited(src_verts_num, false);
    /* Walks from `vert` away through `edge` until reaching a vertex that doesn't dissolve.
     * Returns that end vertex, or -1 if the chain loops back on itself. */
    auto walk_chain_end = [&](const int vert, const int edge, Vector<int> &r_edges) {
      int current_vert = vert;
      int current_edge = edge;
      while (true) {
        r_edges.append(current_edge);
        const int2 verts = src_edges[current_edge];
        const int next_vert = (verts[0] == current_vert) ? verts[1] : verts[0];
        if (!vert_candidate[next_vert]) {
          return next_vert;
        }
        if (visited[next_vert]) {
          /* The chain is a closed cycle. */
          return -1;
        }
        visited[next_vert] = true;
        const int2 next_edges = candidate_edges[next_vert];
        current_edge = (next_edges[0] == current_edge) ? next_edges[1] : next_edges[0];
        current_vert = next_vert;
      }
    };
    Vector<int> chain_edges;
    for (const int vert : IndexRange(src_verts_num)) {
      if (!vert_candidate[vert] || visited[vert]) {
        continue;
      }
      visited[vert] = true;
      chain_edges.clear();
      const int end_a = walk_chain_end(vert, candidate_edges[vert][0], chain_edges);
      const int end_b = walk_chain_end(vert, candidate_edges[vert][1], chain_edges);
      if (end_a == -1 || end_b == -1 || end_a == end_b) {
        /* Cyclic or degenerate chains are not dissolved. */
        continue;
      }
      const int reused_edge = *std::min_element(chain_edges.begin(), chain_edges.end());
      for (const int edge : chain_edges) {
        if (edge != reused_edge) {
          edge_kept[edge] = false;
          chain_edge_map.add(edge, reused_edge);
        }
      }
      reused_edge_verts.add(reused_edge, int2(end_a, end_b));
      for (const int edge : chain_edges) {
        const int2 verts = src_edges[edge];
        for (const int chain_vert : {verts[0], verts[1]}) {
          if (vert_candidate[chain_vert]) {
            vert_dissolved[chain_vert] = true;
          }
        }
      }
    }
  }

  /* Vertices survive if they are used by a remaining edge, or were loose to begin with. */
  Array<bool> vert_kept(src_verts_num, false);
  threading::parallel_for(IndexRange(src_verts_num), 4096, [&](const IndexRange range) {
    for (const int vert : range) {
      if (vert_to_edge_map[vert].is_empty()) {
        vert_kept[vert] = true;
      }
    }
  });
  threading::parallel_for(src_edges.index_range(), 4096, [&](const IndexRange range) {
    for (const int edge : range) {
      if (!edge_kept[edge]) {
        continue;
      }
      const int2 verts = reused_edge_verts.lookup_default(edge, src_edges[edge]);
      vert_kept[verts[0]] = true;
      vert_kept[verts[1]] = true;
    }
  });

  /* Determine the output faces and count their corners. A count of zero means the face is
   * removed: either it merged into another face, or it degenerated below three corners. */
  auto merged_region_of_face = [&](const int face) -> const MergedRegion * {
    const int region = face_region[face];
    if (region_offsets[region].size() < 2 || merged_regions[region].failed) {
      return nullptr;
    }
    return &merged_regions[region];
  };
  auto face_is_region_representative = [&](const int face) {
    return region_faces[region_offsets[face_region[face]].start()] == face;
  };
  Array<int> face_corner_counts(src_faces.size(), 0);
  threading::parallel_for(src_faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int face : range) {
      int num = 0;
      if (const MergedRegion *merged = merged_region_of_face(face)) {
        if (!face_is_region_representative(face)) {
          continue;
        }
        for (const int corner : merged->corners) {
          num += !vert_dissolved[corner_verts[corner]];
        }
      }
      else {
        for (const int vert : corner_verts.slice(src_faces[face])) {
          num += !vert_dissolved[vert];
        }
      }
      face_corner_counts[face] = (num >= 3) ? num : 0;
    }
  });

  IndexMaskMemory memory;
  const IndexMask face_mask = IndexMask::from_predicate(
      IndexMask(src_faces.index_range()), memory, [&](const int face) {
        return face_corner_counts[face] > 0;
      });
  const IndexMask edge_mask = IndexMask::from_bools(edge_kept, memory);
  const IndexMask vert_mask = IndexMask::from_bools(vert_kept, memory);

  if (edge_mask.size() == src_edges.size() && face_mask.size() == src_faces.size()) {
    return nullptr;
  }

  Array<int> vert_new_index(src_verts_num, -1);
  vert_mask.foreach_index(
      [&](const int vert, const int index) { vert_new_index[vert] = index; },
      exec_mode::grain_size(4096));
  Array<int> edge_new_index(src_edges.size(), -1);
  edge_mask.foreach_index(
      [&](const int edge, const int index) { edge_new_index[edge] = index; },
      exec_mode::grain_size(4096));

  Array<int> dst_face_offset_data(face_mask.size() + 1);
  face_mask.foreach_index(
      [&](const int face, const int index) {
        dst_face_offset_data[index] = face_corner_counts[face];
      },
      exec_mode::grain_size(4096));
  const OffsetIndices<int> dst_faces = offset_indices::accumulate_counts_to_offsets(
      dst_face_offset_data);

  Mesh *dst_mesh = bke::mesh_new_no_attributes(
      vert_mask.size(), edge_mask.size(), face_mask.size(), 0);
  BKE_mesh_copy_parameters_for_eval(dst_mesh, &src_mesh);
  bke::MutableAttributeAccessor dst_attributes = dst_mesh->attributes_for_write();

  dst_mesh->face_offsets_for_write().copy_from(dst_face_offset_data);
  dst_mesh->corners_num = dst_faces.total_size();

  dst_attributes.add<int2>(".edge_verts", bke::AttrDomain::Edge, bke::AttributeInitConstruct());
  dst_attributes.add<int>(".corner_vert", bke::AttrDomain::Corner, bke::AttributeInitConstruct());
  dst_attributes.add<int>(".corner_edge", bke::AttrDomain::Corner, bke::AttributeInitConstruct());
  MutableSpan<int2> dst_edges = dst_mesh->edges_for_write();
  MutableSpan<int> dst_corner_verts = dst_mesh->corner_verts_for_write();
  MutableSpan<int> dst_corner_edges = dst_mesh->corner_edges_for_write();

  edge_mask.foreach_index(
      [&](const int edge, const int index) {
        const int2 verts = reused_edge_verts.lookup_default(edge, src_edges[edge]);
        dst_edges[index] = int2(vert_new_index[verts[0]], vert_new_index[verts[1]]);
      },
      exec_mode::grain_size(2048));

  /* Write the corners of every output face, dropping corners of dissolved vertices and remapping
   * edges merged into chains. Also build the source corner map for attribute propagation. */
  Array<int> corner_src_map(dst_faces.total_size());
  face_mask.foreach_index(
      [&](const int face, const int index) {
        int dst_corner = dst_faces[index].start();
        auto write_corner = [&](const int src_corner, const int src_edge) {
          if (vert_dissolved[corner_verts[src_corner]]) {
            return;
          }
          const int edge = edge_kept[src_edge] ? src_edge : chain_edge_map.lookup(src_edge);
          corner_src_map[dst_corner] = src_corner;
          dst_corner_verts[dst_corner] = vert_new_index[corner_verts[src_corner]];
          dst_corner_edges[dst_corner] = edge_new_index[edge];
          dst_corner++;
        };
        if (const MergedRegion *merged = merged_region_of_face(face)) {
          for (const int i : merged->corners.index_range()) {
            write_corner(merged->corners[i], merged->edges[i]);
          }
        }
        else {
          for (const int corner : src_faces[face]) {
            write_corner(corner, corner_edges[corner]);
          }
        }
        BLI_assert(dst_corner == dst_faces[index].one_after_last());
      },
      exec_mode::grain_size(512));

  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  gather_vert_attributes(src_mesh, attribute_filter, vert_mask, *dst_mesh);
  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Edge,
                         bke::AttrDomain::Edge,
                         bke::attribute_filter_with_skip_ref(attribute_filter, {".edge_verts"}),
                         edge_mask,
                         dst_attributes);
  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Face,
                         bke::AttrDomain::Face,
                         attribute_filter,
                         face_mask,
                         dst_attributes);
  bke::gather_attributes(
      src_attributes,
      bke::AttrDomain::Corner,
      bke::AttrDomain::Corner,
      bke::attribute_filter_with_skip_ref(attribute_filter, {".corner_vert", ".corner_edge"}),
      corner_src_map,
      dst_attributes);

  debug_randomize_mesh_order(dst_mesh);

  return dst_mesh;
}

}  // namespace blender::geometry
