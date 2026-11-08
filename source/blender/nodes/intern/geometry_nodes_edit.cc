/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <bit>

#include "DNA_mesh_types.h"
#include "DNA_node_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BLI_math_vector.hh"

#include "MEM_guardedalloc.h"

#include "NOD_geometry_edit.hh"
#include "NOD_geometry_exec.hh"

namespace blender::nodes::geometry_edit {

/* Hash words, not native byte order, so captures also work after saving on another architecture.
 */
static void hash_word(uint64_t &hash, const uint32_t value)
{
  for (const int shift : {0, 8, 16, 24}) {
    hash = (hash ^ ((value >> shift) & 255)) * uint64_t(1099511628211);
  }
}

MeshSignature mesh_signature(const Mesh &mesh)
{
  MeshSignature result;
  result.verts_num = mesh.verts_num;
  result.edges_num = mesh.edges_num;
  result.faces_num = mesh.faces_num;
  result.topology_hash = uint64_t(14695981039346656037ull);
  result.position_hash = result.topology_hash;
  Array<uint64_t> incidence(mesh.verts_num, result.topology_hash);
  hash_word(result.topology_hash, mesh.verts_num);
  hash_word(result.topology_hash, mesh.edges_num);
  hash_word(result.topology_hash, mesh.faces_num);
  for (const int edge_index : mesh.edges().index_range()) {
    const int2 edge = mesh.edges()[edge_index];
    /* BMesh is allowed to reverse an edge's endpoints without changing its identity. */
    hash_word(result.topology_hash, std::min(edge[0], edge[1]));
    hash_word(result.topology_hash, std::max(edge[0], edge[1]));
    hash_word(incidence[edge[0]], edge_index);
    hash_word(incidence[edge[1]], edge_index);
  }
  const OffsetIndices<int> faces = mesh.faces();
  for (const int face : faces.index_range()) {
    hash_word(result.topology_hash, faces[face].size());
  }
  for (const int corner : mesh.corner_verts().index_range()) {
    const int vertex = mesh.corner_verts()[corner];
    hash_word(result.topology_hash, vertex);
    hash_word(incidence[vertex], mesh.edges_num + corner);
  }
  for (const int edge : mesh.corner_edges()) {
    hash_word(result.topology_hash, edge);
  }
  for (const float3 position : mesh.vert_positions()) {
    uint64_t point_hash = uint64_t(14695981039346656037ull);
    for (const int axis : IndexRange(3)) {
      /* Canonicalize signed zero. */
      const uint32_t word = std::bit_cast<uint32_t>(position[axis] == 0.0f ? 0.0f :
                                                                             position[axis]);
      hash_word(result.position_hash, word);
      hash_word(point_hash, word);
    }
    /* Commutative: a coordinate permutation is not proof of deformation-safe identity. */
    result.position_set_hash += point_hash * point_hash + std::rotl(point_hash, 23);
  }
  const auto metadata = mesh.attributes().lookup_meta_data("id");
  if (metadata && metadata->domain == bke::AttrDomain::Point &&
      metadata->data_type == bke::AttrType::Int32)
  {
    const VArray<int> ids = *mesh.attributes().lookup<int>("id", bke::AttrDomain::Point);
    Array<int> sorted_ids(mesh.verts_num);
    result.identity_hash = uint64_t(14695981039346656037ull);
    for (const int vertex : sorted_ids.index_range()) {
      sorted_ids[vertex] = ids[vertex];
      hash_word(result.identity_hash, uint32_t(ids[vertex]));
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());
    if (!sorted_ids.is_empty() &&
        std::adjacent_find(sorted_ids.begin(), sorted_ids.end()) == sorted_ids.end())
    {
      result.validation = CaptureValidation::ElementID;
      return result;
    }
    result.identity_hash = 0;
  }
  /* An isolated point or indistinguishable incidence (e.g. a lone wire's endpoints) has no
   * deformation-independent identity. Such meshes retain exact-position validation. */
  if (!incidence.is_empty() && !incidence.as_span().contains(uint64_t(14695981039346656037ull))) {
    std::sort(incidence.begin(), incidence.end());
    if (std::adjacent_find(incidence.begin(), incidence.end()) == incidence.end()) {
      result.validation = CaptureValidation::OrderedTopology;
    }
  }
  return result;
}

bool capture_matches_mesh(const MeshSignature &capture, const Mesh &mesh)
{
  const MeshSignature current = mesh_signature(mesh);
  if (capture.verts_num != current.verts_num || capture.edges_num != current.edges_num ||
      capture.faces_num != current.faces_num || capture.topology_hash != current.topology_hash)
  {
    return false;
  }
  if (capture.validation == CaptureValidation::ElementID) {
    return current.validation == CaptureValidation::ElementID &&
           capture.identity_hash == current.identity_hash;
  }
  if (capture.position_hash == current.position_hash) {
    return true;
  }
  return capture.validation == CaptureValidation::OrderedTopology &&
         current.validation == CaptureValidation::OrderedTopology &&
         capture.position_set_hash != current.position_set_hash;
}

MeshSignature capture_signature(const NodeGeometryCapturedSelection &capture)
{
  return {capture.verts_num,
          capture.edges_num,
          capture.faces_num,
          capture.topology_hash,
          capture.position_hash,
          capture.position_set_hash,
          capture.identity_hash,
          CaptureValidation(capture.validation_mode)};
}

void capture_initialize(NodeGeometryCapturedSelection &capture,
                        const MeshSignature &signature,
                        const bke::AttrDomain domain,
                        const Span<int> indices,
                        const Span<int> point_indices)
{
  MEM_SAFE_DELETE(capture.indices);
  MEM_SAFE_DELETE(capture.point_indices);
  capture.indices_num = indices.size();
  capture.point_indices_num = point_indices.size();
  capture.indices = MEM_new_array<int>(indices.size(), __func__);
  capture.point_indices = MEM_new_array<int>(point_indices.size(), __func__);
  MutableSpan(capture.indices, indices.size()).copy_from(indices);
  MutableSpan(capture.point_indices, point_indices.size()).copy_from(point_indices);
  capture.domain = int8_t(domain);
  capture.verts_num = signature.verts_num;
  capture.edges_num = signature.edges_num;
  capture.faces_num = signature.faces_num;
  capture.topology_hash = signature.topology_hash;
  capture.position_hash = signature.position_hash;
  capture.position_set_hash = signature.position_set_hash;
  capture.identity_hash = signature.identity_hash;
  capture.validation_mode = int8_t(signature.validation);
  capture.pivot_mode = int8_t(CapturePivot::Median);
  capture.active_index = -1;
}

void capture_copy(NodeGeometryCapturedSelection &dst, const NodeGeometryCapturedSelection &src)
{
  capture_initialize(dst,
                     capture_signature(src),
                     bke::AttrDomain(src.domain),
                     Span(src.indices, src.indices_num),
                     Span(src.point_indices, src.point_indices_num));
  dst.pivot_mode = src.pivot_mode;
  dst.active_domain = src.active_domain;
  dst.active_index = src.active_index;
  std::copy_n(src.pivot, 3, dst.pivot);
}

class CapturedSelectionField final : public bke::MeshFieldInput {
  MeshSignature signature_;
  Vector<int> indices_;
  bke::AttrDomain domain_;

 public:
  CapturedSelectionField(const MeshSignature &signature,
                         const Span<int> indices,
                         const bke::AttrDomain domain)
      : bke::MeshFieldInput(CPPType::get<bool>(), "Captured Selection"),
        signature_(signature),
        indices_(indices),
        domain_(domain)
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const bke::AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    if (!capture_matches_mesh(signature_, mesh)) {
      return VArray<bool>::from_single(false, mesh.attributes().domain_size(domain));
    }
    Array<bool> selection(mesh.attributes().domain_size(domain_), false);
    for (const int index : indices_) {
      if (selection.index_range().contains(index)) {
        selection[index] = true;
      }
    }
    return mesh.attributes().adapt_domain<bool>(
        VArray<bool>::from_container(std::move(selection)), domain_, domain);
  }

  std::optional<bke::AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return domain_;
  }

  void hash_unique(UniqueHashBytes &hash, fn::FieldHashDeep & /*cache*/) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(signature_.verts_num);
    hash.add(signature_.edges_num);
    hash.add(signature_.faces_num);
    hash.add(signature_.topology_hash);
    hash.add(signature_.position_hash);
    hash.add(signature_.position_set_hash);
    hash.add(signature_.identity_hash);
    hash.add(signature_.validation);
    hash.add(domain_);
    hash.add(indices_.size());
    for (const int index : indices_) {
      hash.add(index);
    }
  }
};

static float3 capture_pivot(const NodeGeometryCapturedSelection &capture, const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  if (CapturePivot(capture.pivot_mode) == CapturePivot::Fixed) {
    return float3(capture.pivot);
  }
  if (CapturePivot(capture.pivot_mode) == CapturePivot::Active) {
    const int index = capture.active_index;
    const bke::AttrDomain domain = bke::AttrDomain(capture.active_domain);
    if (domain == bke::AttrDomain::Point && positions.index_range().contains(index)) {
      return positions[index];
    }
    if (domain == bke::AttrDomain::Edge && mesh.edges().index_range().contains(index)) {
      const int2 edge = mesh.edges()[index];
      return (positions[edge[0]] + positions[edge[1]]) * 0.5f;
    }
    if (domain == bke::AttrDomain::Face && mesh.faces().index_range().contains(index)) {
      float3 center(0);
      const Span<int> vertices = mesh.corner_verts().slice(mesh.faces()[index]);
      for (const int vertex : vertices) {
        center += positions[vertex];
      }
      return center / vertices.size();
    }
  }
  float3 center(0);
  for (const int index : Span(capture.point_indices, capture.point_indices_num)) {
    center += positions[index];
  }
  return center / capture.point_indices_num;
}

void capture_execute(GeoNodeExecParams &params, const bool with_pivot)
{
  const auto &storage = *static_cast<const NodeGeometryCapturedSelection *>(params.node().storage);
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Mesh *mesh = geometry.get_mesh();
  const auto signature = capture_signature(storage);
  const bke::AttrDomain domain = bke::AttrDomain(storage.domain);
  bool valid = mesh && storage.indices_num > 0 && storage.point_indices_num > 0 &&
               ELEM(
                   domain, bke::AttrDomain::Point, bke::AttrDomain::Edge, bke::AttrDomain::Face) &&
               capture_matches_mesh(signature, *mesh);
  if (valid) {
    for (const int index : Span(storage.indices, storage.indices_num)) {
      valid &= index >= 0 && index < mesh->attributes().domain_size(domain);
    }
    for (const int index : Span(storage.point_indices, storage.point_indices_num)) {
      valid &= index >= 0 && index < mesh->verts_num;
    }
  }
  params.set_output("Valid"_ustr, valid);
  if (!valid) {
    params.error_message_add(
        NodeWarningType::Warning,
        "Captured mesh identity changed or is missing; recapture the selection");
    params.set_output("Selection"_ustr, false);
    params.set_output("Points"_ustr, false);
    if (with_pivot) {
      params.set_output("Pivot"_ustr, float3(0));
    }
    return;
  }
  params.set_output("Selection"_ustr,
                    fn::Field<bool>::from_input<CapturedSelectionField>(
                        signature, Span(storage.indices, storage.indices_num), domain));
  params.set_output("Points"_ustr,
                    fn::Field<bool>::from_input<CapturedSelectionField>(
                        signature,
                        Span(storage.point_indices, storage.point_indices_num),
                        bke::AttrDomain::Point));
  if (with_pivot) {
    params.set_output("Pivot"_ustr, capture_pivot(storage, *mesh));
  }
}

}  // namespace blender::nodes::geometry_edit
