/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_customdata.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "GEO_mesh_loop_cut_slide.hh"
#include "GEO_randomize.hh"

#include "bmesh.hh"

namespace blender::geometry {

namespace {

struct RingComponent {
  Vector<int> edges;
  Vector<int> faces;
  bool valid = true;
};

struct RingTopology {
  /** Final accepted component index for every edge, or -1. */
  Array<int> edge_components;
  /** Consistently oriented endpoints for every edge in an accepted component. */
  Array<int2> oriented_edge_verts;
  Vector<RingComponent> components;
  int skipped_rings = 0;
};

static int opposite_quad_corner(const IndexRange face, const int corner)
{
  BLI_assert(face.size() == 4);
  return face[(corner - face.start() + 2) % 4];
}

static int2 propagate_opposite_orientation(const IndexRange face,
                                           const Span<int> corner_verts,
                                           const int corner,
                                           const int opposite_corner,
                                           const int2 current_orientation)
{
  const int next_corner = bke::mesh::face_corner_next(face, corner);
  const int opposite_next = bke::mesh::face_corner_next(face, opposite_corner);
  const int boundary_start = corner_verts[corner];
  const int boundary_end = corner_verts[next_corner];
  const int opposite_start = corner_verts[opposite_corner];
  const int opposite_end = corner_verts[opposite_next];

  if (current_orientation == int2(boundary_start, boundary_end)) {
    return int2(opposite_end, opposite_start);
  }
  if (current_orientation == int2(boundary_end, boundary_start)) {
    return int2(opposite_start, opposite_end);
  }
  return int2(-1, -1);
}

static RingTopology find_edge_rings(const Mesh &mesh, const IndexMask &seed_edges)
{
  const Span<int2> edges = mesh.edges();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  const Array<int> corner_to_face = mesh.corner_to_face_map();

  Array<int> edge_to_corner_offsets;
  Array<int> edge_to_corner_indices;
  const GroupedSpan<int> edge_to_corners = bke::mesh::build_edge_to_corner_map(
      corner_edges, edges.size(), edge_to_corner_offsets, edge_to_corner_indices);

  Array<int> candidate_by_edge(edges.size(), -1);
  Array<int2> oriented_edge_verts(edges.size(), int2(-1, -1));
  Array<int> face_visit_stamp(faces.size(), -1);
  Vector<RingComponent> candidates;

  seed_edges.foreach_index([&](const int seed_edge) {
    if (!edges.index_range().contains(seed_edge) || candidate_by_edge[seed_edge] != -1) {
      return;
    }

    const int candidate_i = candidates.size();
    RingComponent component;
    Vector<int> queue;
    queue.append(seed_edge);
    candidate_by_edge[seed_edge] = candidate_i;
    oriented_edge_verts[seed_edge] = edges[seed_edge];

    for (int queue_i = 0; queue_i < queue.size(); queue_i++) {
      const int edge = queue[queue_i];
      component.edges.append(edge);
      if (edge_to_corners[edge].size() > 2) {
        component.valid = false;
      }

      for (const int corner : edge_to_corners[edge]) {
        const int face_i = corner_to_face[corner];
        const IndexRange face = faces[face_i];
        if (face.size() != 4) {
          continue;
        }
        if (face_visit_stamp[face_i] != candidate_i) {
          face_visit_stamp[face_i] = candidate_i;
          component.faces.append(face_i);
        }

        const int opposite_corner = opposite_quad_corner(face, corner);
        const int opposite_edge = corner_edges[opposite_corner];
        const int2 orientation = propagate_opposite_orientation(
            face, corner_verts, corner, opposite_corner, oriented_edge_verts[edge]);
        if (orientation[0] == -1) {
          component.valid = false;
          continue;
        }

        if (candidate_by_edge[opposite_edge] == -1) {
          candidate_by_edge[opposite_edge] = candidate_i;
          oriented_edge_verts[opposite_edge] = orientation;
          queue.append(opposite_edge);
        }
        else if (candidate_by_edge[opposite_edge] != candidate_i ||
                 oriented_edge_verts[opposite_edge] != orientation)
        {
          /* A non-orientable or branching ring has no stable signed slide direction. */
          component.valid = false;
        }
      }
    }

    if (component.faces.is_empty()) {
      component.valid = false;
    }
    candidates.append(std::move(component));
  });

  RingTopology topology;
  topology.edge_components.reinitialize(edges.size());
  topology.edge_components.fill(-1);
  topology.oriented_edge_verts = std::move(oriented_edge_verts);

  /* Accept components in seed order. Two components touching the same quad are perpendicular
   * rings. Supporting that case would require two loop indices on their crossing edges. */
  Array<int> face_owner(faces.size(), -1);
  for (RingComponent &candidate : candidates) {
    if (candidate.valid) {
      for (const int face_i : candidate.faces) {
        if (face_owner[face_i] != -1) {
          candidate.valid = false;
          break;
        }
      }
    }

    if (!candidate.valid) {
      topology.skipped_rings++;
      continue;
    }

    const int component_i = topology.components.size();
    for (const int edge : candidate.edges) {
      topology.edge_components[edge] = component_i;
    }
    for (const int face_i : candidate.faces) {
      face_owner[face_i] = component_i;
    }
    topology.components.append(std::move(candidate));
  }
  return topology;
}

struct SourceEdgeInfo {
  int edge;
  int component;
  BMVert *vert_a;
  BMVert *vert_b;
};

static float cut_fraction(const int cut, const int cuts, const float factor)
{
  return float(cut + 1 + std::clamp(factor, -1.0f, 1.0f)) / float(cuts + 1);
}

static BMLoop *find_source_face_loop(BMVert *vert,
                                     const int source_face_offset,
                                     const int source_face)
{
  BMIter iter;
  BMLoop *loop;
  BM_ITER_ELEM (loop, &iter, vert, BM_LOOPS_OF_VERT) {
    if (BM_ELEM_CD_GET_INT(loop->f, source_face_offset) == source_face + 1) {
      return loop;
    }
  }
  return nullptr;
}

static void interpolate_corner_data(BMesh &bm,
                                    BMVert &vert_a,
                                    BMVert &vert_b,
                                    BMVert &new_vert,
                                    const Span<int> source_faces,
                                    const int source_face_offset,
                                    const float factor)
{
  for (const int source_face : source_faces) {
    const BMLoop *loop_a = find_source_face_loop(&vert_a, source_face_offset, source_face);
    const BMLoop *loop_b = find_source_face_loop(&vert_b, source_face_offset, source_face);
    if (!loop_a || !loop_b) {
      continue;
    }

    const void *src[2] = {loop_a->head.data, loop_b->head.data};
    const float weights[2] = {1.0f - factor, factor};
    BMIter iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &iter, &new_vert, BM_LOOPS_OF_VERT) {
      if (BM_ELEM_CD_GET_INT(loop->f, source_face_offset) == source_face + 1) {
        CustomData_bmesh_interp(&bm.ldata, src, weights, 2, loop->head.data);
      }
    }
  }
}

static Vector<BMVert *> ordered_split_vertices(const SourceEdgeInfo &source,
                                               const Span<BMEdge *> segments,
                                               const int cuts)
{
  Vector<BMVert *> result;
  result.reserve(cuts);
  BMVert *current = source.vert_a;
  BMEdge *previous_edge = nullptr;

  for (const int segment_i : IndexRange(cuts + 1)) {
    BMEdge *next_edge = nullptr;
    for (BMEdge *edge : segments) {
      if (edge != previous_edge && BM_vert_in_edge(edge, current)) {
        if (next_edge != nullptr) {
          return {};
        }
        next_edge = edge;
      }
    }
    if (!next_edge) {
      return {};
    }

    BMVert *next_vert = BM_edge_other_vert(next_edge, current);
    if (segment_i < cuts) {
      if (next_vert == source.vert_b) {
        return {};
      }
      result.append(next_vert);
    }
    else if (next_vert != source.vert_b) {
      return {};
    }
    previous_edge = next_edge;
    current = next_vert;
  }
  return result;
}

static void add_loop_output_attributes(BMesh &bm,
                                       const StringRef loop_index_attribute,
                                       const StringRef loops_attribute,
                                       const Map<BMVert *, int> &loop_index_by_vert,
                                       const int source_edge_layer)
{
  if (!loop_index_attribute.is_empty()) {
    BM_data_layer_add_named(&bm, &bm.edata, CD_PROP_INT32, loop_index_attribute);
  }
  if (!loops_attribute.is_empty()) {
    BM_data_layer_add_named(&bm, &bm.edata, CD_PROP_BOOL, loops_attribute);
  }

  /* Adding custom-data layers can change offsets, so resolve every offset after both additions. */
  const int source_edge_offset = CustomData_get_n_offset(
      &bm.edata, CD_PROP_INT32, source_edge_layer);
  const int loop_index_offset = loop_index_attribute.is_empty() ?
                                    -1 :
                                    CustomData_get_offset_named(
                                        &bm.edata, CD_PROP_INT32, loop_index_attribute);
  const int loops_offset = loops_attribute.is_empty() ?
                               -1 :
                               CustomData_get_offset_named(
                                   &bm.edata, CD_PROP_BOOL, loops_attribute);

  BMIter iter;
  BMEdge *edge;
  BM_ITER_MESH (edge, &iter, &bm, BM_EDGES_OF_MESH) {
    int loop_index = -1;
    /* Split segments retain their source-edge ID. New loop edges do not. */
    if (BM_ELEM_CD_GET_INT(edge, source_edge_offset) == 0) {
      const int *index_a = loop_index_by_vert.lookup_ptr(edge->v1);
      const int *index_b = loop_index_by_vert.lookup_ptr(edge->v2);
      if (index_a && index_b && *index_a == *index_b) {
        loop_index = *index_a;
      }
    }
    if (loop_index_offset != -1) {
      BM_ELEM_CD_SET_INT(edge, loop_index_offset, loop_index);
    }
    if (loops_offset != -1) {
      BM_ELEM_CD_SET_BOOL(edge, loops_offset, loop_index >= 0);
    }
  }
}

}  // namespace

LoopCutSlidePreview preview_loop_cut_and_slide_mesh(const Mesh &mesh,
                                                    const IndexMask &seed_edges,
                                                    const int cuts,
                                                    const float factor)
{
  BLI_assert(cuts >= 1);
  const RingTopology topology = find_edge_rings(mesh, seed_edges);
  LoopCutSlidePreview preview;
  preview.skipped_rings = topology.skipped_rings;
  const Span<float3> positions = mesh.vert_positions();
  for (const int component_i : topology.components.index_range()) {
    const RingComponent &component = topology.components[component_i];
    for (const int face_i : component.faces) {
      const IndexRange face = mesh.faces()[face_i];
      for (const int corner : face) {
        const int edge = mesh.corner_edges()[corner];
        if (topology.edge_components[edge] != component_i) {
          continue;
        }
        const int opposite_edge = mesh.corner_edges()[opposite_quad_corner(face, corner)];
        const int2 a = topology.oriented_edge_verts[edge];
        const int2 b = topology.oriented_edge_verts[opposite_edge];
        for (const int cut : IndexRange(cuts)) {
          const float t = cut_fraction(cut, cuts, factor);
          preview.lines.append(math::interpolate(positions[a[0]], positions[a[1]], t));
          preview.lines.append(math::interpolate(positions[b[0]], positions[b[1]], t));
        }
        break;
      }
    }
  }
  return preview;
}

LoopCutSlideResult loop_cut_and_slide_mesh(const Mesh &mesh,
                                           const IndexMask &seed_edges,
                                           const int cuts,
                                           const float factor,
                                           const StringRef loop_index_attribute,
                                           const StringRef loops_attribute)
{
  BLI_assert(cuts >= 1);
  RingTopology topology = find_edge_rings(mesh, seed_edges);
  LoopCutSlideResult result;
  result.skipped_rings = topology.skipped_rings;
  if (topology.components.is_empty()) {
    return result;
  }

  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMeshFromMeshParams from_mesh_params{};
  from_mesh_params.calc_face_normal = true;
  from_mesh_params.calc_vert_normal = true;
  BMesh *bm = BKE_mesh_to_bmesh_ex(&mesh, &create_params, &from_mesh_params);
  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);

  /* Unnamed temporary integer layers cannot collide with user attributes. The source IDs are
   * copied by BMesh when edges and faces are split, then removed before conversion back to Mesh.
   */
  const int source_edge_layer = CustomData_number_of_layers(&bm->edata, CD_PROP_INT32);
  BM_data_layer_add(bm, &bm->edata, CD_PROP_INT32);
  const int source_edge_offset = CustomData_get_n_offset(
      &bm->edata, CD_PROP_INT32, source_edge_layer);
  const int source_face_layer = CustomData_number_of_layers(&bm->pdata, CD_PROP_INT32);
  BM_data_layer_add(bm, &bm->pdata, CD_PROP_INT32);
  const int source_face_offset = CustomData_get_n_offset(
      &bm->pdata, CD_PROP_INT32, source_face_layer);

  for (const int face : mesh.faces().index_range()) {
    BM_ELEM_CD_SET_INT(bm->ftable[face], source_face_offset, face + 1);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_EDGE, BM_ELEM_TAG, false);
  Vector<SourceEdgeInfo> source_edges;
  for (const int edge : mesh.edges().index_range()) {
    const int component = topology.edge_components[edge];
    if (component == -1) {
      continue;
    }
    BMEdge *bm_edge = bm->etable[edge];
    BM_ELEM_CD_SET_INT(bm_edge, source_edge_offset, edge + 1);
    BM_elem_flag_enable(bm_edge, BM_ELEM_TAG);
    const int2 verts = topology.oriented_edge_verts[edge];
    source_edges.append({edge, component, bm->vtable[verts[0]], bm->vtable[verts[1]]});
  }

  BM_mesh_esubdivide(bm,
                     BM_ELEM_TAG,
                     0.0f,
                     SUBD_FALLOFF_INVSQUARE,
                     true,
                     0.0f,
                     0.0f,
                     cuts,
                     SUBDIV_SELECT_NONE,
                     SUBD_CORNER_PATH,
                     false,
                     true,
                     false,
                     0);

  Map<int, Vector<BMEdge *>> segments_by_source_edge;
  BMIter edge_iter;
  BMEdge *edge;
  BM_ITER_MESH (edge, &edge_iter, bm, BM_EDGES_OF_MESH) {
    const int source_edge = BM_ELEM_CD_GET_INT(edge, source_edge_offset) - 1;
    if (source_edge >= 0) {
      segments_by_source_edge.lookup_or_add_default(source_edge).append(edge);
    }
  }

  Array<int> edge_to_corner_offsets;
  Array<int> edge_to_corner_indices;
  const GroupedSpan<int> edge_to_corners = bke::mesh::build_edge_to_corner_map(
      mesh.corner_edges(), mesh.edges_num, edge_to_corner_offsets, edge_to_corner_indices);
  const Array<int> corner_to_face = mesh.corner_to_face_map();

  Map<BMVert *, int> loop_index_by_vert;
  for (const SourceEdgeInfo &source : source_edges) {
    const Vector<BMEdge *> *segments = segments_by_source_edge.lookup_ptr(source.edge);
    if (!segments) {
      BLI_assert_unreachable();
      continue;
    }
    const Vector<BMVert *> split_verts = ordered_split_vertices(source, *segments, cuts);
    if (split_verts.size() != cuts) {
      BLI_assert_unreachable();
      continue;
    }

    Vector<int, 2> source_faces;
    for (const int corner : edge_to_corners[source.edge]) {
      source_faces.append_non_duplicates(corner_to_face[corner]);
    }

    for (const int cut : IndexRange(cuts)) {
      BMVert &vert = *split_verts[cut];
      const float slide_factor = cut_fraction(cut, cuts, factor);
      copy_v3_v3(
          vert.co,
          math::interpolate(float3(source.vert_a->co), float3(source.vert_b->co), slide_factor));
      BM_data_interp_from_verts(bm, source.vert_a, source.vert_b, &vert, slide_factor);
      interpolate_corner_data(*bm,
                              *source.vert_a,
                              *source.vert_b,
                              vert,
                              source_faces,
                              source_face_offset,
                              slide_factor);

      const int loop_index = source.component * cuts + cut;
      if (const int *existing = loop_index_by_vert.lookup_ptr(&vert)) {
        BLI_assert(*existing == loop_index);
      }
      else {
        loop_index_by_vert.add_new(&vert, loop_index);
      }
    }
  }

  if (!loop_index_attribute.is_empty() || !loops_attribute.is_empty()) {
    add_loop_output_attributes(
        *bm, loop_index_attribute, loops_attribute, loop_index_by_vert, source_edge_layer);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_EDGE, BM_ELEM_TAG, false);
  BM_data_layer_free_n(bm, &bm->edata, CD_PROP_INT32, source_edge_layer);
  BM_data_layer_free_n(bm, &bm->pdata, CD_PROP_INT32, source_face_layer);
  BM_mesh_normals_update(bm);

  BMeshToMeshParams to_mesh_params{};
  to_mesh_params.calc_object_remap = false;
  result.mesh = BKE_mesh_from_bmesh_nomain(bm, &to_mesh_params, &mesh);
  BM_mesh_free(bm);

  debug_randomize_mesh_order(result.mesh);
  return result;
}

}  // namespace blender::geometry
