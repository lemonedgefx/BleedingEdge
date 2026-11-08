/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <optional>

#include "BKE_attribute.hh"

#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_loop_cut_slide.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_loop_cut_slide_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh in which to insert edge loops");
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(false)
      .hide_value()
      .evaluated_geometry_field()
      .description("Seed edges whose quad rings receive loop cuts");
  b.add_input<decl::Int>("Cuts"_ustr)
      .default_value(1)
      .min(1)
      .max(100)
      .description("Number of loops to insert in every selected edge ring");
  b.add_input<decl::Float>("Factor"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Signed amount to slide the new loops between their neighboring loops");

  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry();
  b.add_output<decl::Bool>("Loops"_ustr)
      .anonymous_attribute_output()
      .description("Selection of all edge loops created by the node");
  b.add_output<decl::Int>("Loop Index"_ustr)
      .anonymous_attribute_output()
      .description("Unique index of each created edge loop, or -1 for all other edges");
}

static void fill_default_loop_indices(Mesh &mesh, const StringRef attribute_name)
{
  bke::SpanAttributeWriter<int> indices =
      mesh.attributes_for_write().lookup_or_add_for_write_only_span<int>(attribute_name,
                                                                         bke::AttrDomain::Edge);
  indices.span.fill(-1);
  indices.finish();
}

static void fill_default_loops(Mesh &mesh, const StringRef attribute_name)
{
  bke::SpanAttributeWriter<bool> loops =
      mesh.attributes_for_write().lookup_or_add_for_write_only_span<bool>(attribute_name,
                                                                          bke::AttrDomain::Edge);
  loops.span.fill(false);
  loops.finish();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);
  const int cuts = std::clamp(params.extract_input<int>("Cuts"_ustr), 1, 1000);
  const float factor = std::clamp(params.extract_input<float>("Factor"_ustr), -1.0f, 1.0f);
  const std::optional<std::string> loops_id = params.get_output_anonymous_attribute_id_if_needed(
      "Loops"_ustr);
  const std::optional<std::string> loop_index_id =
      params.get_output_anonymous_attribute_id_if_needed("Loop Index"_ustr);

  int skipped_rings = 0;
  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *mesh = geometry_set.get_mesh();
    if (!mesh) {
      return;
    }

    const bke::MeshFieldContext field_context{*mesh, bke::AttrDomain::Edge};
    fn::FieldEvaluator evaluator{field_context, mesh->edges_num};
    evaluator.set_selection(selection_field);
    evaluator.evaluate();
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();

    if (!selection.is_empty()) {
      geometry::LoopCutSlideResult result = geometry::loop_cut_and_slide_mesh(
          *mesh,
          selection,
          cuts,
          factor,
          loop_index_id ? StringRef(*loop_index_id) : StringRef(),
          loops_id ? StringRef(*loops_id) : StringRef());
      skipped_rings += result.skipped_rings;
      if (result.mesh) {
        geometry_set.replace_mesh(result.mesh);
        return;
      }
    }

    /* Anonymous outputs must still have their documented fallback when the selection is empty or
     * contains no valid quad ring. */
    if (loop_index_id) {
      fill_default_loop_indices(*geometry_set.get_mesh_for_write(), *loop_index_id);
    }
    if (loops_id) {
      fill_default_loops(*geometry_set.get_mesh_for_write(), *loops_id);
    }
  });

  if (skipped_rings > 0) {
    params.error_message_add(
        NodeWarningType::Warning,
        TIP_("Some selected edge rings were invalid or intersected another selected ring"));
  }
  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeLoopCutSlide"_ustr);
  ntype.ui_name = "Loop Cut and Slide";
  ntype.ui_description = "Insert and slide edge loops through selected quad rings";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_loop_cut_slide_cc
