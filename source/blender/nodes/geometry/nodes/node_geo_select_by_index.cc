/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <optional>
#include <utility>

#include "BLO_read_write.hh"
#include "NOD_geometry_edit.hh"
#include "NOD_rna_define.hh"

#include "DNA_mesh_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_select_by_index_cc {

NODE_STORAGE_FUNCS(NodeGeometryCapturedSelection)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const bool captured = node && node->custom2 == geometry_edit::SELECT_BY_INDEX_CAPTURED;
  b.add_input<decl::String>("Indices"_ustr)
      .available(!captured)
      .description("Comma-separated element indices and ranges, for example 0, 3, 8-12, 20");
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .available(captured)
      .description("The mesh input stage at which this selection was captured");
  b.add_output<decl::Bool>("Selection"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("Elements whose indices are listed in the input");
  b.add_output<decl::Bool>("Points"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("Selected vertices, including all vertices of selected edges or faces");
  b.add_output<decl::Bool>("Valid"_ustr)
      .description(
          "Valid index syntax in Manual mode, or matching mesh identity in Captured mode");
  b.add_output<decl::Vector>("Pivot"_ustr)
      .available(captured)
      .description(
          "Selection-based transform pivot, recalculated from the captured input geometry");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode &node = *ptr->data_as<bNode>();
  const bool captured = node.custom2 == geometry_edit::SELECT_BY_INDEX_CAPTURED;
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &domain_row = layout.row(true);
  domain_row.enabled_set(!captured);
  domain_row.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
  if (captured && node.storage) {
    layout.label(fmt::format("{} selected", node_storage(node).indices_num), ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(AttrDomain::Point);
  node->custom2 = geometry_edit::SELECT_BY_INDEX_MANUAL;
  node->storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
}

static void node_free(bNode *node)
{
  if (!node->storage) {
    return;
  }
  auto &storage = node_storage(*node);
  MEM_SAFE_DELETE(storage.indices);
  MEM_SAFE_DELETE(storage.point_indices);
  MEM_delete(&storage);
}

static void node_copy(bNodeTree * /*tree*/, bNode *dst, const bNode *src)
{
  dst->storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
  if (src->storage) {
    geometry_edit::capture_copy(node_storage(*dst), node_storage(*src));
  }
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  if (node.storage) {
    const auto &storage = node_storage(node);
    writer.write_int32_array(storage.indices_num, storage.indices);
    writer.write_int32_array(storage.point_indices_num, storage.point_indices);
  }
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  /* Existing manually authored Select by Index nodes had no storage. */
  if (!node.storage) {
    node.storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
  }
  auto &storage = node_storage(node);
  BLO_read_array_and_validate_size(&reader, &storage.indices, &storage.indices_num);
  BLO_read_array_and_validate_size(&reader, &storage.point_indices, &storage.point_indices_num);
}

struct IndexInterval {
  int first;
  int last;
};

struct ParsedIndices {
  Vector<IndexInterval> intervals;
  bool has_invalid_token = false;
};

static std::optional<int> parse_index(const StringRef text)
{
  int value;
  const std::from_chars_result parse_result = std::from_chars(text.begin(), text.end(), value);
  if (parse_result.ec != std::errc() || parse_result.ptr != text.end() || value < 0) {
    return std::nullopt;
  }
  return value;
}

static ParsedIndices parse_indices(const StringRef text)
{
  ParsedIndices result;
  StringRef remaining = text;
  while (!remaining.is_empty()) {
    const int64_t separator = remaining.find(',');
    const StringRef token = (separator == -1 ? remaining : remaining.substr(0, separator)).trim();
    if (!token.is_empty()) {
      const int64_t range_separator = token.find('-');
      if (range_separator == -1) {
        if (const std::optional<int> value = parse_index(token)) {
          result.intervals.append({*value, *value});
        }
        else {
          result.has_invalid_token = true;
        }
      }
      else {
        const StringRef first_text = token.substr(0, range_separator).trim();
        const StringRef last_text = token.substr(range_separator + 1).trim();
        const std::optional<int> first = parse_index(first_text);
        const std::optional<int> last = parse_index(last_text);
        if (first && last && *first <= *last && last_text.find('-') == -1) {
          result.intervals.append({*first, *last});
        }
        else {
          result.has_invalid_token = true;
        }
      }
    }
    else {
      result.has_invalid_token = true;
    }
    if (separator == -1) {
      break;
    }
    remaining = remaining.substr(separator + 1);
  }

  std::sort(result.intervals.begin(),
            result.intervals.end(),
            [](const IndexInterval &a, const IndexInterval &b) {
              return a.first < b.first || (a.first == b.first && a.last < b.last);
            });

  Vector<IndexInterval> merged_intervals;
  for (const IndexInterval interval : result.intervals) {
    if (merged_intervals.is_empty() ||
        int64_t(interval.first) > int64_t(merged_intervals.last().last) + 1)
    {
      merged_intervals.append(interval);
    }
    else {
      merged_intervals.last().last = std::max(merged_intervals.last().last, interval.last);
    }
  }
  result.intervals = std::move(merged_intervals);
  return result;
}

class SelectByIndexFieldInput final : public bke::MeshFieldInput {
 private:
  Vector<IndexInterval> intervals_;
  AttrDomain domain_;
  bool points_;

 public:
  SelectByIndexFieldInput(Vector<IndexInterval> intervals,
                          const AttrDomain domain,
                          const bool points = false)
      : bke::MeshFieldInput(CPPType::get<bool>(), "Select Elements"),
        intervals_(std::move(intervals)),
        domain_(domain),
        points_(points)
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const int64_t domain_size = mesh.attributes().domain_size(domain_);
    if (domain_size == 0 || intervals_.is_empty()) {
      return {};
    }

    Array<bool> selection(domain_size, false);
    for (const IndexInterval interval : intervals_) {
      if (interval.first >= domain_size) {
        break;
      }
      const int64_t last = std::min<int64_t>(interval.last, domain_size - 1);
      selection.as_mutable_span().slice(interval.first, last - interval.first + 1).fill(true);
    }
    if (points_ && domain_ != AttrDomain::Point) {
      Array<bool> vertices(mesh.verts_num, false);
      for (const int index : selection.index_range()) {
        if (!selection[index]) {
          continue;
        }
        if (domain_ == AttrDomain::Edge) {
          const int2 edge = mesh.edges()[index];
          vertices[edge[0]] = vertices[edge[1]] = true;
        }
        else {
          for (const int vertex : mesh.corner_verts().slice(mesh.faces()[index])) {
            vertices[vertex] = true;
          }
        }
      }
      return mesh.attributes().adapt_domain<bool>(
          VArray<bool>::from_container(std::move(vertices)), AttrDomain::Point, domain);
    }
    return mesh.attributes().adapt_domain<bool>(
        VArray<bool>::from_container(std::move(selection)), domain_, domain);
  }

  void hash_unique(UniqueHashBytes &hash, fn::FieldHashDeep & /*deep_hash_cache*/) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(domain_);
    hash.add(points_);
    hash.add(intervals_.size());
    for (const IndexInterval interval : intervals_) {
      hash.add(interval.first);
      hash.add(interval.last);
    }
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return points_ ? AttrDomain::Point : domain_;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.node().custom2 == geometry_edit::SELECT_BY_INDEX_CAPTURED) {
    geometry_edit::capture_execute(params, true);
    return;
  }
  const std::string indices_text = params.extract_input<std::string>("Indices"_ustr);
  ParsedIndices indices = parse_indices(indices_text);
  if (indices.has_invalid_token) {
    params.error_message_add(NodeWarningType::Warning,
                             "Some index entries are invalid and were ignored");
  }

  const AttrDomain domain = AttrDomain(params.node().custom1);
  params.set_output("Valid"_ustr, !indices.has_invalid_token);
  params.set_output(
      "Points"_ustr,
      Field<bool>::from_input<SelectByIndexFieldInput>(indices.intervals, domain, true));
  params.set_output(
      "Selection"_ustr,
      Field<bool>::from_input<SelectByIndexFieldInput>(std::move(indices.intervals), domain));
}

static void node_rna(StructRNA *srna)
{
  static const EnumPropertyItem mode_items[] = {
      {geometry_edit::SELECT_BY_INDEX_MANUAL,
       "MANUAL",
       0,
       "Manual",
       "Enter element indices or ranges"},
      {geometry_edit::SELECT_BY_INDEX_CAPTURED,
       "CAPTURED",
       0,
       "Captured",
       "Use a validated viewport selection"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_node_enum(
      srna,
      "mode",
      "Mode",
      "Selection source",
      mode_items,
      EnumRNAAccessors([](PointerRNA *ptr,
                          PropertyRNA * /*prop*/) { return int(ptr->data_as<bNode>()->custom2); },
                       [](PointerRNA *ptr, PropertyRNA * /*prop*/, const int value) {
                         bNode &node = *ptr->data_as<bNode>();
                         if (!node.storage) {
                           node.storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
                         }
                         if (value == geometry_edit::SELECT_BY_INDEX_CAPTURED) {
                           if (node_storage(node).indices_num == 0) {
                             node_storage(node).domain = int8_t(node.custom1);
                           }
                           node.custom1 = int16_t(node_storage(node).domain);
                         }
                         node.custom2 = int16_t(value);
                       }),
      geometry_edit::SELECT_BY_INDEX_MANUAL);
  static const EnumPropertyItem domain_items[] = {
      {int(AttrDomain::Point), "POINT", 0, "Point", "Select mesh vertices by index"},
      {int(AttrDomain::Edge), "EDGE", 0, "Edge", "Select mesh edges by index"},
      {int(AttrDomain::Face), "FACE", 0, "Face", "Select mesh faces by index"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(
      srna,
      "domain",
      "Domain",
      "Mesh element type addressed by the indices",
      domain_items,
      EnumRNAAccessors([](PointerRNA *ptr,
                          PropertyRNA * /*prop*/) { return int(ptr->data_as<bNode>()->custom1); },
                       [](PointerRNA *ptr, PropertyRNA * /*prop*/, int value) {
                         bNode &node = *ptr->data_as<bNode>();
                         if (node.custom2 == geometry_edit::SELECT_BY_INDEX_MANUAL) {
                           node.custom1 = int16_t(value);
                         }
                       }),
      int(AttrDomain::Point));
  PropertyRNA *prop = RNA_def_property(srna, "selected_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs_runtime(
      prop,
      [](PointerRNA *ptr, PropertyRNA * /*prop*/) {
        const bNode &node = *ptr->data_as<bNode>();
        return node.storage && node.custom2 == geometry_edit::SELECT_BY_INDEX_CAPTURED ?
                   node_storage(node).indices_num :
                   0;
      },
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSelectByIndex"_ustr);
  ntype.ui_name = "Select Elements";
  ntype.ui_description = "Select mesh indices manually or use a validated viewport capture";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = node_init;
  bke::node_type_storage(ntype, "NodeGeometryCapturedSelection", node_free, node_copy);
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_180;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_select_by_index_cc
