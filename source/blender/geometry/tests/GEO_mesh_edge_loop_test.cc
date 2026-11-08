/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "DNA_mesh_types.h"

#include "GEO_mesh_edge_loop.hh"
#include "GEO_mesh_primitive_grid.hh"

#include "testing/testing.h"

namespace blender::geometry::tests {

class EdgeLoopTest : public bke::BlenderGTestBase {
 public:
  Mesh *mesh = nullptr;

  void TearDown() override
  {
    if (mesh != nullptr) {
      BKE_id_free(nullptr, mesh);
    }
  }
};

static void expect_selected_edges(const Span<bool> selection, const Span<int> expected)
{
  Vector<int> selected_edges;
  for (const int edge : selection.index_range()) {
    if (selection[edge]) {
      selected_edges.append(edge);
    }
  }
  EXPECT_EQ_SPAN(expected, selected_edges.as_span());
}

static void expect_selected_faces(const Span<bool> selection, const Span<int> expected)
{
  Vector<int> selected_faces;
  for (const int face : selection.index_range()) {
    if (selection[face]) {
      selected_faces.append(face);
    }
  }
  EXPECT_EQ_SPAN(expected, selected_faces.as_span());
}

TEST_F(EdgeLoopTest, InteriorQuadLoop)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({4}, memory);

  const Array<bool> selection = edge_loop_selection(*mesh, seeds);

  expect_selected_edges(selection.as_span(), {3, 4, 5});
}

TEST_F(EdgeLoopTest, MultipleStartEdges)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({4, 16}, memory);

  const Array<bool> selection = edge_loop_selection(*mesh, seeds);

  expect_selected_edges(selection.as_span(), {3, 4, 5, 15, 16, 17});
}

TEST_F(EdgeLoopTest, BoundaryStopsAtOuterCornersByDefault)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({0}, memory);

  const Array<bool> selection = edge_loop_selection(*mesh, seeds);

  expect_selected_edges(selection.as_span(), {0, 1, 2});
}

TEST_F(EdgeLoopTest, BoundaryContinuesAroundCornersWhenAllowed)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({0}, memory);

  const Array<bool> selection = edge_loop_selection(*mesh, seeds, EdgeLoopDelimit::Ngon);

  expect_selected_edges(selection.as_span(), {0, 1, 2, 9, 10, 11, 12, 13, 14, 21, 22, 23});
}

TEST_F(EdgeLoopTest, SharpEdgeDelimiter)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  bke::SpanAttributeWriter<bool> sharp_edges =
      mesh->attributes_for_write().lookup_or_add_for_write_span<bool>("sharp_edge",
                                                                      bke::AttrDomain::Edge);
  sharp_edges.span.fill(false);
  sharp_edges.span[3] = true;
  sharp_edges.finish();

  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({4}, memory);
  const Array<bool> selection = edge_loop_selection(
      *mesh, seeds, edge_loop_delimit_default | EdgeLoopDelimit::Sharp);

  expect_selected_edges(selection.as_span(), {4, 5});
}

TEST_F(EdgeLoopTest, ConnectedWireEdges)
{
  mesh = BKE_mesh_new_nomain(5, 4, 0, 0);
  MutableSpan<int2> edges = mesh->edges_for_write();
  edges[0] = int2(0, 1);
  edges[1] = int2(1, 2);
  edges[2] = int2(1, 3);
  edges[3] = int2(3, 4);

  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({0}, memory);
  const Array<bool> selection = edge_loop_selection(*mesh, seeds);

  expect_selected_edges(selection.as_span(), {0, 1, 2, 3});
}

TEST_F(EdgeLoopTest, FaceLoopU)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({4}, memory);

  const Array<bool> selection = face_loop_selection(*mesh, seeds, FaceLoopDirection::U);

  expect_selected_faces(selection.as_span(), {1, 4, 7});
}

TEST_F(EdgeLoopTest, FaceLoopV)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({4}, memory);

  const Array<bool> selection = face_loop_selection(*mesh, seeds, FaceLoopDirection::V);

  expect_selected_faces(selection.as_span(), {3, 4, 5});
}

TEST_F(EdgeLoopTest, FaceLoopMultipleSeeds)
{
  mesh = create_grid_mesh(4, 4, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask seeds = IndexMask::from_indices<int>({3, 4}, memory);

  const Array<bool> selection = face_loop_selection(*mesh, seeds, FaceLoopDirection::U);

  expect_selected_faces(selection.as_span(), {0, 1, 3, 4, 6, 7});
}

}  // namespace blender::geometry::tests
