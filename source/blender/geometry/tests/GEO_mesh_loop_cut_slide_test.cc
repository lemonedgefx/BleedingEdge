/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"

#include "BLI_math_vector.hh"

#include "DNA_mesh_types.h"

#include "GEO_mesh_loop_cut_slide.hh"
#include "GEO_mesh_primitive_grid.hh"

#include "testing/testing.h"

namespace blender::geometry::tests {

class LoopCutSlideTest : public bke::BlenderGTestBase {
 public:
  Mesh *input = nullptr;
  Mesh *output = nullptr;

  void TearDown() override
  {
    if (input != nullptr) {
      BKE_id_free(nullptr, input);
    }
    if (output != nullptr) {
      BKE_id_free(nullptr, output);
    }
  }
};

static IndexMask edge_selection(const std::initializer_list<int> edges, IndexMaskMemory &memory)
{
  return IndexMask::from_indices<int>(edges, memory);
}

TEST_F(LoopCutSlideTest, SingleCutSlidesAndInterpolatesAttributes)
{
  input = create_grid_mesh(2, 2, 2.0f, 2.0f, StringRef("uv"));
  {
    bke::SpanAttributeWriter<float> weights =
        input->attributes_for_write().lookup_or_add_for_write_only_span<float>(
            "weight", bke::AttrDomain::Point);
    const Span<float3> positions = input->vert_positions();
    for (const int vert : positions.index_range()) {
      weights.span[vert] = (positions[vert].y + 1.0f) * 0.5f;
    }
    weights.finish();
  }

  IndexMaskMemory memory;
  LoopCutSlideResult result = loop_cut_and_slide_mesh(
      *input, edge_selection({0}, memory), 1, 0.5f, "loop_index", "loops");
  output = result.mesh;

  ASSERT_NE(output, nullptr);
  EXPECT_EQ(result.skipped_rings, 0);
  EXPECT_EQ(output->verts_num, 6);
  EXPECT_EQ(output->edges_num, 7);
  EXPECT_EQ(output->faces_num, 2);
  EXPECT_EQ(output->corners_num, 8);

  const VArraySpan<int> loop_indices = *output->attributes().lookup<int>("loop_index",
                                                                         bke::AttrDomain::Edge);
  const VArraySpan<bool> loops = *output->attributes().lookup<bool>("loops",
                                                                    bke::AttrDomain::Edge);
  const VArraySpan<float> weights = *output->attributes().lookup<float>("weight",
                                                                        bke::AttrDomain::Point);
  const VArraySpan<float2> uvs = *output->attributes().lookup<float2>("uv",
                                                                      bke::AttrDomain::Corner);
  const Span<int2> edges = output->edges();
  const Span<float3> positions = output->vert_positions();

  int loop_edge = -1;
  for (const int edge : edges.index_range()) {
    if (loop_indices[edge] == 0) {
      ASSERT_EQ(loop_edge, -1);
      loop_edge = edge;
      EXPECT_TRUE(loops[edge]);
    }
    else {
      EXPECT_EQ(loop_indices[edge], -1);
      EXPECT_FALSE(loops[edge]);
    }
  }
  ASSERT_NE(loop_edge, -1);

  const int2 loop_verts = edges[loop_edge];
  for (const int vert : {loop_verts[0], loop_verts[1]}) {
    EXPECT_NEAR(positions[vert].y, 0.5f, 1e-6f);
    EXPECT_NEAR(weights[vert], 0.75f, 1e-6f);
  }
  for (const int corner : output->corner_verts().index_range()) {
    if (ELEM(output->corner_verts()[corner], loop_verts[0], loop_verts[1])) {
      EXPECT_NEAR(uvs[corner].y, 0.75f, 1e-6f);
    }
  }
}

TEST_F(LoopCutSlideTest, MultipleCutsHaveStableLoopIndices)
{
  input = create_grid_mesh(2, 2, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  LoopCutSlideResult result = loop_cut_and_slide_mesh(
      *input, edge_selection({0}, memory), 2, 0.0f, "loop_index");
  output = result.mesh;

  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->verts_num, 8);
  EXPECT_EQ(output->edges_num, 10);
  EXPECT_EQ(output->faces_num, 3);
  EXPECT_EQ(output->corners_num, 12);

  const VArraySpan<int> loop_indices = *output->attributes().lookup<int>("loop_index",
                                                                         bke::AttrDomain::Edge);
  int index_0_num = 0;
  int index_1_num = 0;
  for (const int index : loop_indices) {
    index_0_num += index == 0;
    index_1_num += index == 1;
    EXPECT_TRUE(ELEM(index, -1, 0, 1));
  }
  EXPECT_EQ(index_0_num, 1);
  EXPECT_EQ(index_1_num, 1);
}

TEST_F(LoopCutSlideTest, LoopsSelectionDoesNotRequireLoopIndexOutput)
{
  input = create_grid_mesh(2, 2, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  LoopCutSlideResult result = loop_cut_and_slide_mesh(
      *input, edge_selection({0}, memory), 2, 0.0f, {}, "loops");
  output = result.mesh;

  ASSERT_NE(output, nullptr);
  EXPECT_FALSE(output->attributes().contains("loop_index"));
  const VArraySpan<bool> loops = *output->attributes().lookup<bool>("loops",
                                                                    bke::AttrDomain::Edge);
  EXPECT_EQ(std::count(loops.begin(), loops.end(), true), 2);
}

TEST_F(LoopCutSlideTest, IntersectingRingIsSkipped)
{
  input = create_grid_mesh(2, 2, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  LoopCutSlideResult result = loop_cut_and_slide_mesh(
      *input, edge_selection({0, 2}, memory), 1, 0.0f, "loop_index");
  output = result.mesh;

  ASSERT_NE(output, nullptr);
  EXPECT_EQ(result.skipped_rings, 1);
  EXPECT_EQ(output->faces_num, 2);
  const VArraySpan<int> loop_indices = *output->attributes().lookup<int>("loop_index",
                                                                         bke::AttrDomain::Edge);
  EXPECT_EQ(std::count(loop_indices.begin(), loop_indices.end(), 0), 1);
}

static void expect_preview_matches_output(const LoopCutSlidePreview &preview, const Mesh &mesh)
{
  const VArraySpan<bool> loops = *mesh.attributes().lookup<bool>("loops", bke::AttrDomain::Edge);
  ASSERT_EQ(std::count(loops.begin(), loops.end(), true), preview.lines.size() / 2);
  Array<bool> matched(mesh.edges_num, false);
  const Span<float3> positions = mesh.vert_positions();
  for (int i = 0; i < preview.lines.size(); i += 2) {
    bool found = false;
    for (const int edge : mesh.edges().index_range()) {
      if (!loops[edge] || matched[edge]) {
        continue;
      }
      const int2 verts = mesh.edges()[edge];
      const auto near = [&](const float3 &a, const float3 &b) {
        return math::distance_squared(a, b) < 1.0e-10f;
      };
      if ((near(preview.lines[i], positions[verts[0]]) &&
           near(preview.lines[i + 1], positions[verts[1]])) ||
          (near(preview.lines[i], positions[verts[1]]) &&
           near(preview.lines[i + 1], positions[verts[0]])))
      {
        matched[edge] = found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(LoopCutSlideTest, EveryGridSeedPreviewMatchesSignedCutKernel)
{
  input = create_grid_mesh(5, 3, 4.0f, 2.0f, StringRef("uv"));
  const Array<float3> original(input->vert_positions());
  for (const int seed : input->edges().index_range()) {
    for (const int cuts : {1, 3}) {
      for (const float factor : {-1.0f, -0.4f, 0.0f, 0.65f, 1.0f}) {
        IndexMaskMemory memory;
        const IndexMask selection = edge_selection({seed}, memory);
        const LoopCutSlidePreview preview = preview_loop_cut_and_slide_mesh(
            *input, selection, cuts, factor);
        const LoopCutSlideResult result = loop_cut_and_slide_mesh(
            *input, selection, cuts, factor, "loop_index", "loops");
        ASSERT_NE(result.mesh, nullptr);
        EXPECT_EQ(preview.skipped_rings, result.skipped_rings);
        expect_preview_matches_output(preview, *result.mesh);
        BKE_id_free(nullptr, result.mesh);
      }
    }
  }
  EXPECT_EQ(input->vert_positions(), original.as_span());
}

TEST_F(LoopCutSlideTest, IntersectingRingPreviewUsesSameAcceptance)
{
  input = create_grid_mesh(2, 2, 2.0f, 2.0f, std::nullopt);
  IndexMaskMemory memory;
  const IndexMask selection = edge_selection({0, 2}, memory);
  const LoopCutSlidePreview preview = preview_loop_cut_and_slide_mesh(*input, selection, 2, 0.4f);
  const LoopCutSlideResult result = loop_cut_and_slide_mesh(
      *input, selection, 2, 0.4f, "loop_index", "loops");
  output = result.mesh;
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(preview.skipped_rings, 1);
  EXPECT_EQ(preview.skipped_rings, result.skipped_rings);
  expect_preview_matches_output(preview, *output);
}

TEST_F(LoopCutSlideTest, WireSeedAndEmptySelectionDoNotPreviewACut)
{
  input = BKE_mesh_new_nomain(2, 1, 0, 0);
  input->vert_positions_for_write()[0] = float3(0, 0, 0);
  input->vert_positions_for_write()[1] = float3(0, 0, 1);
  input->edges_for_write()[0] = int2(0, 1);
  IndexMaskMemory memory;
  const LoopCutSlidePreview wire = preview_loop_cut_and_slide_mesh(
      *input, edge_selection({0}, memory), 1, 0.0f);
  EXPECT_TRUE(wire.lines.is_empty());
  EXPECT_EQ(wire.skipped_rings, 1);
  EXPECT_TRUE(preview_loop_cut_and_slide_mesh(*input, IndexMask(), 1, 0.0f).lines.is_empty());
}

}  // namespace blender::geometry::tests
