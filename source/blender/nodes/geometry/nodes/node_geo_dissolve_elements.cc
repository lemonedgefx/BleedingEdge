/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_rna_define.hh"

#include "DNA_mesh_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_dissolve.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_dissolve_elements_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.add_input<decl::Geometry>("Mesh"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh whose elements to dissolve");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry().align_with_previous();
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field()
      .description("The elements to dissolve");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(AttrDomain::Point);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);
  const AttrDomain domain = AttrDomain(params.node().custom1);

  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *mesh = geometry_set.get_mesh();
    if (!mesh) {
      return;
    }
    const bke::MeshFieldContext field_context{*mesh, domain};
    fn::FieldEvaluator evaluator{field_context, mesh->attributes().domain_size(domain)};
    evaluator.set_selection(selection_field);
    evaluator.evaluate();
    const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
    if (selection.is_empty()) {
      return;
    }

    if (Mesh *result = geometry::dissolve_mesh_elements(
            *mesh, domain, selection, params.get_attribute_filter("Mesh"_ustr)))
    {
      geometry_set.replace_mesh(result);
    }
  });

  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static const EnumPropertyItem domain_items[] = {
      {int(AttrDomain::Point),
       "POINT",
       0,
       "Point",
       "Remove selected vertices, merging the faces around them"},
      {int(AttrDomain::Edge),
       "EDGE",
       0,
       "Edge",
       "Remove selected edges, merging the faces on both sides"},
      {int(AttrDomain::Face),
       "FACE",
       0,
       "Face",
       "Merge connected groups of selected faces into single faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "Which type of element to dissolve",
                    domain_items,
                    NOD_inline_enum_accessors(custom1),
                    int(AttrDomain::Point));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDissolveElements"_ustr);
  ntype.ui_name = "Dissolve Elements";
  ntype.ui_description =
      "Remove selected mesh elements, merging the surrounding geometry to fill the gaps";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_dissolve_elements_cc
