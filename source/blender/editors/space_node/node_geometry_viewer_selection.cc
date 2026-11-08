/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_compute_context_cache.hh"
#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_geometry_set.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_node_tree_zones.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_geometry_viewer.hh"
#include "ED_mesh.hh"
#include "ED_node.hh"
#include "ED_numinput.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GEO_mesh_loop_cut_slide.hh"
#include "GEO_mesh_triangulate.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "NOD_eval_log.hh"
#include "NOD_geometry_edit.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "bmesh.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

using bke::AttrDomain;

static void session_end_by_uid(bContext &C, uint32_t uid);

namespace {

enum class EditRecipe {
  Offset,
  UniformScale,
  AxisScale,
  Matrix,
  Extrude,
  LoopCut,
  Delete,
  Dissolve,
  Merge,
  Duplicate,
  Triangulate,
  FlipFaces,
  ShadeSmooth
};

static bool edit_recipe_is_face_tool(const EditRecipe recipe)
{
  return ELEM(recipe, EditRecipe::Triangulate, EditRecipe::FlipFaces, EditRecipe::ShadeSmooth);
}

struct EditRecipeLink {
  int from_node;
  std::string from_socket;
  int to_node;
  std::string to_socket;
  int multi_input_sort_id = -1;
};

/* Only an edit session's own last translation may be updated in place. Never infer ownership
 * from an arbitrary Set Position node already connected to the Viewer. */
struct GeometryViewerMoveBatch {
  int capture_node_identifier = 0;
  int operation_node_identifier = 0;
  AttrDomain domain = AttrDomain::Point;
  nodes::geometry_edit::MeshSignature capture_signature;
  Vector<int> indices;
  Vector<int> point_indices;
  Vector<EditRecipeLink> links;
  float3 offset = float3(0);
};

struct GeometryViewerSelectionSession {
  uint32_t active_object_session_uid = 0;
  uint32_t source_object_session_uid = 0;
  Vector<uint32_t> selected_object_session_uids;
  short previous_select_mode = SCE_SELECT_VERTEX;
  bool node_editor_was_pinned = false;
  int verts_num = 0;
  int edges_num = 0;
  int faces_num = 0;
  uint32_t scene_session_uid = 0;
  uint32_t tree_session_uid = 0;
  uint32_t proxy_mesh_session_uid = 0;
  std::string view_layer_name;
  int modifier_uid = 0;
  int viewer_identifier = 0;
  int source_node_identifier = 0;
  std::string source_socket_identifier;
  std::string viewer_socket_identifier;
  /* Compare this token with live editors before dereferencing. Never own an editor pointer. */
  uintptr_t owner_space_token = 0;
  int frame = 0;
  float subframe = 0;
  float4x4 source_to_world = float4x4::identity();
  bool editing = false;
  nodes::geometry_edit::MeshSignature source_signature;
  nodes::geometry_edit::MeshSignature proxy_signature;
  Vector<float3> positions;
  Vector<int> picked_indices;
  Vector<int> point_indices;
  Vector<int> capture_indices;
  Vector<int> capture_point_indices;
  Vector<int> selected_node_identifiers;
  int active_node_identifier = 0;
  int active_element_index = -1;
  int active_element_type = 0;
  float3 pivot = float3(0);
  float3 offset = float3(0);
  uint64_t generation = 0;
  int capture_node_identifier = 0;
  int operation_node_identifier = 0;
  /* A temporary Top/Loops-field consumer, removed before committing the ordinary operation. */
  int preview_node_identifier = 0;
  std::string top_attribute_name;
  bool extrusion_armed = false;
  bool extrusion_preparing = false;
  bool extrude_individual = false;
  bool duplication_preparing = false;
  bool duplication_armed = false;
  int duplicate_join_node_identifier = 0;
  bool merge_preparing = false;
  GeometryNodeMergeByDistanceMode merge_mode = GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL;
  float merge_distance = 0.001f;
  bool face_tool_preparing = false;
  bool face_smooth = true;
  int triangulate_quad_method = int(geometry::TriangulateQuadMode::Beauty);
  int triangulate_ngon_method = int(geometry::TriangulateNGonMode::Beauty);
  bool loop_cut_preparing = false;
  bool loop_cut_sliding = false;
  bool deletion_preparing = false;
  GeometryNodeDeleteGeometryMode delete_mode = GEO_NODE_DELETE_GEOMETRY_MODE_ALL;
  int loop_cut_cuts = 1;
  float loop_cut_factor = 0;
  nodes::geometry_edit::MeshSignature loop_cut_preview_signature;
  Vector<int> helper_node_identifiers;
  Vector<EditRecipeLink> recipe_links;
  std::optional<float2> viewer_original_location;
  float2 viewer_layout_location = float2(0);
  float2 recipe_layout_origin = float2(0);
  EditRecipe recipe = EditRecipe::Offset;
  AttrDomain edit_domain = AttrDomain::Point;
  float4x4 local_transform = float4x4::identity();
  GeometryViewerTransform transform_kind = GeometryViewerTransform::Move;
  float3 rotation_euler = float3(0);
  bool rotation_group_preparing = false;
  uint32_t rotation_group_tree_uid = 0;
  int rotation_group_node_identifier = 0;
  std::string rotation_socket_identifier;
  std::optional<GeometryViewerMoveBatch> move_batch;
  /* Borrowed committed nodes must be restored, not deleted, on gesture cancellation. */
  bool reusing_move = false;
  /* Active native transform or loop-cut tool. Compare only with live handlers on shutdown. */
  wmOperator *native_operator = nullptr;
  bool native_invalidated = false;
  bool native_end_requested = false;
  bool native_in_callback = false;
  Vector<int> hidden_verts, hidden_edges, hidden_faces;
  eNodeLink_Flag original_link_flag = eNodeLink_Flag(0);
  ComputeContextHash compute_context_hash;
};

struct GeometryViewerEditResume {
  GeometryViewerSelectionSession session;
  int result_node_identifier;
  Vector<float3> positions;
  bool node_layout = false;
};

static std::optional<GeometryViewerEditResume> &edit_resume()
{
  static std::optional<GeometryViewerEditResume> resume;
  return resume;
}

/* Undo decoding clears ordinary resume state while loading Main. This separately owned, ID-based
 * layout snapshot survives that cleanup only until the matching undo post callback. */
static std::optional<GeometryViewerEditResume> &layout_undo_resume()
{
  static std::optional<GeometryViewerEditResume> resume;
  return resume;
}

static Map<uint32_t, GeometryViewerSelectionSession> &selection_sessions()
{
  static Map<uint32_t, GeometryViewerSelectionSession> sessions;
  return sessions;
}
/* Runtime IDs can be resurrected by a memfile undo step made by another editor. */
static Vector<uint32_t> &retired_proxy_uids()
{
  static Vector<uint32_t> uids;
  return uids;
}
static Vector<uint32_t> &retired_proxy_mesh_uids()
{
  static Vector<uint32_t> uids;
  return uids;
}

static void edit_transaction_rollback(bContext &C, GeometryViewerSelectionSession &session);

static Object *find_object(Main &bmain, const uint32_t session_uid)
{
  if (session_uid == 0) {
    return nullptr;
  }
  return id_cast<Object *>(BKE_libblock_find_session_uid(&bmain, ID_OB, session_uid));
}

static Mesh *find_mesh(Main &bmain, const uint32_t session_uid)
{
  if (session_uid == 0) {
    return nullptr;
  }
  return id_cast<Mesh *>(BKE_libblock_find_session_uid(&bmain, ID_ME, session_uid));
}

static bNodeTree *find_tree(Main &bmain, const uint32_t uid)
{
  bNodeTree *tree = id_cast<bNodeTree *>(BKE_libblock_find_session_uid(&bmain, ID_NT, uid));
  if (tree) {
    tree->ensure_topology_cache();
  }
  return tree;
}

static SpaceNode *find_owner_space(Main &bmain,
                                   const GeometryViewerSelectionSession &session,
                                   const bool active_only = false)
{
  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &space : area.spacedata) {
        if (space.spacetype == SPACE_NODE && uintptr_t(&space) == session.owner_space_token &&
            (!active_only || &space == area.spacedata.first()))
        {
          return reinterpret_cast<SpaceNode *>(&space);
        }
      }
    }
  }
  return nullptr;
}

static bNode *viewer_node_from_operator(bContext &C, wmOperator &op)
{
  SpaceNode *snode = CTX_wm_space_node(&C);
  if (!snode || !snode->edittree) {
    return nullptr;
  }
  const int node_identifier = RNA_int_get(op.ptr, "node_identifier");
  bNode *node = node_identifier != 0 ? snode->edittree->node_by_id(node_identifier) :
                                       bke::node_get_active(*snode->edittree);
  if (!node || node->type_legacy != GEO_NODE_VIEWER) {
    return nullptr;
  }
  return node;
}

static bool geometry_viewer_selection_poll(bContext *C)
{
  if (!ED_operator_node_editable(C)) {
    return false;
  }
  const SpaceNode *snode = CTX_wm_space_node(C);
  return snode && snode->edittree && snode->edittree->type == NTREE_GEOMETRY;
}

static const bke::GeometrySet *viewer_geometry_get(const SpaceNode &snode,
                                                   const bNode &viewer_node)
{
  nodes::eval_log::ContextualNodeTreeLogs tree_logs =
      nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(snode);
  nodes::eval_log::NodeTreeLog *tree_log = tree_logs.get_main_tree_log(viewer_node);
  if (!tree_log) {
    return nullptr;
  }
  tree_log->ensure_viewer_node_logs();
  nodes::eval_log::ViewerNodeLog *viewer_log = tree_log->viewer_node_logs.lookup_default(
      viewer_node.identifier, nullptr);
  if (!viewer_log) {
    return nullptr;
  }
  return viewer_log->main_geometry();
}

static const Mesh *viewer_mesh_get(const SpaceNode &snode, const bNode &viewer_node)
{
  const bke::GeometrySet *geometry = viewer_geometry_get(snode, viewer_node);
  return geometry ? geometry->get_mesh() : nullptr;
}

static nodes::geometry_edit::MeshSignature proxy_signature_get(BMesh &bm)
{
  Mesh *mesh = BKE_id_new_nomain<Mesh>(nullptr);
  BM_mesh_bm_to_me_for_eval(bm, *mesh, nullptr);
  const auto signature = nodes::geometry_edit::mesh_signature(*mesh);
  BKE_id_free(nullptr, mesh);
  return signature;
}

static bNodeLink *viewer_geometry_link(bNodeTree &tree, const bNode &viewer)
{
  bNodeLink *result = nullptr;
  for (bNodeLink &link : tree.links) {
    if (link.tonode == &viewer && link.tosock->type == SOCK_GEOMETRY) {
      if (result || (link.flag & NODE_LINK_MUTED)) {
        return nullptr;
      }
      result = &link;
    }
  }
  return result;
}

static short selection_mode_from_domain(const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
      return SCE_SELECT_VERTEX;
    case AttrDomain::Edge:
      return SCE_SELECT_EDGE;
    case AttrDomain::Face:
      return SCE_SELECT_FACE;
    default:
      return 0;
  }
}

static std::optional<AttrDomain> selection_domain_from_mode(const short select_mode)
{
  if (select_mode == SCE_SELECT_VERTEX) {
    return AttrDomain::Point;
  }
  if (select_mode == SCE_SELECT_EDGE) {
    return AttrDomain::Edge;
  }
  if (select_mode == SCE_SELECT_FACE) {
    return AttrDomain::Face;
  }
  /* Mesh Edit Mode can combine modes with Shift. Prefer the most specific enabled domain. */
  if (select_mode & SCE_SELECT_FACE) {
    return AttrDomain::Face;
  }
  if (select_mode & SCE_SELECT_EDGE) {
    return AttrDomain::Edge;
  }
  if (select_mode & SCE_SELECT_VERTEX) {
    return AttrDomain::Point;
  }
  return std::nullopt;
}

static GeometryViewerSelectionSession selection_session_create(Main &bmain,
                                                               Scene &scene,
                                                               ViewLayer &view_layer,
                                                               const SpaceNode &snode,
                                                               const Object &source_object,
                                                               const Mesh &mesh)
{
  GeometryViewerSelectionSession session;
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  if (const Base *active_base = BKE_view_layer_active_base_get(&view_layer)) {
    session.active_object_session_uid = active_base->object->id.session_uid;
  }
  for (const Base &base : *BKE_view_layer_object_bases_get(&view_layer)) {
    if (base.flag & BASE_SELECTED) {
      session.selected_object_session_uids.append(base.object->id.session_uid);
    }
  }
  session.source_object_session_uid = source_object.id.session_uid;
  session.previous_select_mode = scene.toolsettings->selectmode;
  session.node_editor_was_pinned = (snode.flag & SNODE_PIN) != 0;
  session.verts_num = mesh.verts_num;
  session.edges_num = mesh.edges_num;
  session.faces_num = mesh.faces_num;
  session.scene_session_uid = scene.id.session_uid;
  session.view_layer_name = view_layer.name;
  session.tree_session_uid = snode.edittree->id.session_uid;
  session.owner_space_token = uintptr_t(&snode);
  session.frame = scene.r.cfra;
  session.subframe = scene.r.subframe;
  session.source_to_world = source_object.object_to_world();
  session.source_signature = nodes::geometry_edit::mesh_signature(mesh);
  session.positions = Vector<float3>(mesh.vert_positions());
  return session;
}

static void selection_context_restore(bContext &C,
                                      SpaceNode *snode,
                                      const GeometryViewerSelectionSession &session)
{
  Main &bmain = *CTX_data_main(&C);
  Scene *saved_scene = id_cast<Scene *>(
      BKE_libblock_find_session_uid(&bmain, ID_SCE, session.scene_session_uid));
  ViewLayer *saved_layer = saved_scene ?
                               BKE_view_layer_find(saved_scene, session.view_layer_name.c_str()) :
                               nullptr;
  if (!saved_scene || !saved_layer) {
    return;
  }
  Scene &scene = *saved_scene;
  ViewLayer &view_layer = *saved_layer;

  scene.toolsettings->selectmode = session.previous_select_mode;
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  BKE_view_layer_base_deselect_all(bmain, &scene, &view_layer);

  for (const uint32_t session_uid : session.selected_object_session_uids) {
    if (Object *object = find_object(bmain, session_uid)) {
      if (Base *base = BKE_view_layer_base_find(&view_layer, object)) {
        ed::object::base_select(base, ed::object::BA_SELECT);
      }
    }
  }

  Object *active_object = find_object(bmain, session.active_object_session_uid);
  if (!active_object) {
    active_object = find_object(bmain, session.source_object_session_uid);
  }
  Base *active_base = active_object ? BKE_view_layer_base_find(&view_layer, active_object) :
                                      nullptr;
  if (CTX_data_view_layer(&C) == &view_layer) {
    ed::object::base_activate(&C, active_base);
  }
  else {
    view_layer.basact = active_base;
  }

  if (snode) {
    if (session.node_editor_was_pinned) {
      snode->flag |= SNODE_PIN;
    }
    else {
      snode->flag &= ~SNODE_PIN;
      if (CTX_wm_space_node(&C) == snode) {
        snode_set_context(C);
      }
    }
  }

  WM_event_add_notifier(&C, NC_SCENE | ND_OB_SELECT, &scene);
  WM_event_add_notifier(&C, NC_SCENE | ND_MODE, &scene);
}

static void temporary_selection_object_delete(Main &bmain, Scene &scene, Object &object)
{
  retired_proxy_uids().append_non_duplicates(object.id.session_uid);
  const uint32_t mesh_session_uid = object.data ? static_cast<ID *>(object.data)->session_uid : 0;
  retired_proxy_mesh_uids().append_non_duplicates(mesh_session_uid);
  if (object.mode & OB_MODE_EDIT) {
    ed::object::editmode_exit_ex(&bmain, &scene, &object, ed::object::EM_FREEDATA);
  }
  BKE_id_delete(&bmain, &object.id);
  if (Mesh *mesh = find_mesh(bmain, mesh_session_uid)) {
    BKE_id_delete(&bmain, &mesh->id);
  }
  DEG_relations_tag_update(&bmain);
}

static std::optional<GeometryViewerSelectionSession> selection_session_take(
    const uint32_t object_session_uid)
{
  GeometryViewerSelectionSession *session = selection_sessions().lookup_ptr(object_session_uid);
  if (!session) {
    return std::nullopt;
  }
  std::optional<GeometryViewerSelectionSession> result(std::move(*session));
  selection_sessions().remove(object_session_uid);
  return result;
}

static void selection_session_finish(bContext &C, SpaceNode &snode, bNode &viewer_node)
{
  Main &bmain = *CTX_data_main(&C);
  Scene &scene = *CTX_data_scene(&C);
  NodeGeometryViewer &storage = *static_cast<NodeGeometryViewer *>(viewer_node.storage);
  const uint32_t object_session_uid = storage.selection_object_session_uid;
  if (auto *session = selection_sessions().lookup_ptr(object_session_uid)) {
    if (session->native_operator) {
      session_end_by_uid(C, object_session_uid);
      return;
    }
  }
  std::optional<GeometryViewerSelectionSession> session = selection_session_take(
      object_session_uid);

  if (session && session->editing) {
    edit_transaction_rollback(C, *session);
  }

  if (Object *object = find_object(bmain, object_session_uid)) {
    if (BMEditMesh *edit_mesh = BKE_editmesh_from_object(object)) {
      if (const std::optional<AttrDomain> domain = selection_domain_from_mode(
              edit_mesh->selectmode))
      {
        storage.selection_domain = int8_t(*domain);
      }
    }
    temporary_selection_object_delete(bmain, scene, *object);
  }
  else if (session) {
    if (Mesh *mesh = find_mesh(bmain, session->proxy_mesh_session_uid)) {
      BKE_id_delete(&bmain, &mesh->id);
    }
  }

  storage.selection_enabled = false;
  storage.editing_enabled = false;
  storage.selection_object_session_uid = 0;

  if (session) {
    selection_context_restore(C, &snode, *session);
  }
  WM_event_add_notifier(&C, NC_NODE | ND_DISPLAY, snode.edittree);
}

static bool selection_topology_is_unchanged(BMesh &bm,
                                            const GeometryViewerSelectionSession &session)
{
  return bm.totvert == session.verts_num && bm.totedge == session.edges_num &&
         bm.totface == session.faces_num && proxy_signature_get(bm) == session.proxy_signature;
}

static Vector<int> selected_indices_get(BMesh &bm,
                                        const AttrDomain domain,
                                        const char flag = BM_ELEM_SELECT)
{
  Vector<int> indices;
  BMIter iter;
  switch (domain) {
    case AttrDomain::Point: {
      BM_mesh_elem_index_ensure(&bm, BM_VERT);
      BMVert *vert;
      BM_ITER_MESH (vert, &iter, &bm, BM_VERTS_OF_MESH) {
        if (BM_elem_flag_test(vert, flag)) {
          indices.append(BM_elem_index_get(vert));
        }
      }
      break;
    }
    case AttrDomain::Edge: {
      BM_mesh_elem_index_ensure(&bm, BM_EDGE);
      BMEdge *edge;
      BM_ITER_MESH (edge, &iter, &bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(edge, flag)) {
          indices.append(BM_elem_index_get(edge));
        }
      }
      break;
    }
    case AttrDomain::Face: {
      BM_mesh_elem_index_ensure(&bm, BM_FACE);
      BMFace *face;
      BM_ITER_MESH (face, &iter, &bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(face, flag)) {
          indices.append(BM_elem_index_get(face));
        }
      }
      break;
    }
    default:
      break;
  }
  std::sort(indices.begin(), indices.end());
  return indices;
}

static std::string selected_indices_format(const Span<int> indices)
{
  std::string text;
  for (int64_t i = 0; i < indices.size();) {
    const int first = indices[i];
    int last = first;
    i++;
    while (i < indices.size() && indices[i] == last + 1) {
      last = indices[i];
      i++;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += std::to_string(first);
    if (last != first) {
      text += "-";
      text += std::to_string(last);
    }
  }
  return text;
}

static void capture_indices_text_update(bNode &node)
{
  const auto &storage = *static_cast<const NodeGeometryCapturedSelection *>(node.storage);
  const std::string text = selected_indices_format(Span(storage.indices, storage.indices_num));
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, "Indices"_ustr)) {
    auto &value = socket->default_value_typed<bNodeSocketValueString>()->value;
    /* Never expose a truncated manual selection when the structured capture is larger. */
    STRNCPY_UTF8(value, text.size() < sizeof(value) ? text.c_str() : "");
  }
}

static wmOperatorStatus selection_enable(bContext &C,
                                         wmOperator &op,
                                         bNode &viewer_node,
                                         const bool editing = false)
{
  Main &bmain = *CTX_data_main(&C);
  Scene &scene = *CTX_data_scene(&C);
  ViewLayer &view_layer = *CTX_data_view_layer(&C);
  SpaceNode &snode = *CTX_wm_space_node(&C);
  NodeGeometryViewer &storage = *static_cast<NodeGeometryViewer *>(viewer_node.storage);

  if (CTX_data_mode_enum(&C) != CTX_MODE_OBJECT) {
    BKE_report(op.reports,
               RPT_ERROR,
               "Viewer selection can only be enabled while Blender is in Object Mode");
    return OPERATOR_CANCELLED;
  }
  if (!selection_sessions().is_empty()) {
    BKE_report(op.reports, RPT_ERROR, "Disable the active Viewer session before starting another");
    return OPERATOR_CANCELLED;
  }

  const std::optional<ObjectAndModifier> object_and_modifier =
      get_geometry_nodes_modifier_for_node_editor(snode);
  if (!object_and_modifier) {
    BKE_report(
        op.reports, RPT_ERROR, "Viewer selection requires a Geometry Nodes modifier context");
    return OPERATOR_CANCELLED;
  }

  const Mesh *viewer_mesh = viewer_mesh_get(snode, viewer_node);
  if (!viewer_mesh) {
    BKE_report(op.reports, RPT_ERROR, "The Viewer does not currently contain a mesh");
    return OPERATOR_CANCELLED;
  }

  bNodeLink *source_link = viewer_geometry_link(*snode.edittree, viewer_node);
  if (editing) {
    const bke::GeometrySet *geometry = viewer_geometry_get(snode, viewer_node);
    if (!geometry || geometry->has_instances() || geometry->has_curves() ||
        geometry->has_pointcloud() || geometry->has_volume() || geometry->has_grease_pencil())
    {
      BKE_report(op.reports,
                 RPT_ERROR,
                 "Viewer editing requires one realized mesh, without other geometry components");
      return OPERATOR_CANCELLED;
    }
    const bke::bNodeTreeZones *zones = snode.edittree->zones();
    if (snode.edittree != object_and_modifier->nmd->node_group || !source_link || !zones ||
        zones->get_zone_by_node(viewer_node.identifier) ||
        zones->get_zone_by_socket(*source_link->fromsock))
    {
      BKE_report(op.reports,
                 RPT_ERROR,
                 "Viewer editing requires a single linked geometry input in the modifier's root "
                 "tree, outside zones");
      return OPERATOR_CANCELLED;
    }
  }

  const AttrDomain domain = AttrDomain(storage.selection_domain);
  const short select_mode = selection_mode_from_domain(domain);
  if (select_mode == 0) {
    BKE_report(op.reports, RPT_ERROR, "Unsupported mesh selection domain");
    return OPERATOR_CANCELLED;
  }

  GeometryViewerSelectionSession session = selection_session_create(
      bmain, scene, view_layer, snode, *object_and_modifier->object, *viewer_mesh);
  session.editing = editing;
  session.viewer_identifier = viewer_node.identifier;
  session.modifier_uid = object_and_modifier->nmd->modifier.persistent_uid;
  bke::ComputeContextCache context_cache;
  if (const ComputeContext *context = compute_context_for_edittree(snode, context_cache)) {
    session.compute_context_hash = context->hash();
  }
  if (source_link) {
    session.source_node_identifier = source_link->fromnode->identifier;
    session.source_socket_identifier = source_link->fromsock->identifier;
    session.viewer_socket_identifier = source_link->tosock->identifier;
    session.original_link_flag = source_link->flag;
  }

  const char *proxy_name = editing ? "Viewer Editing" : "Viewer Selection";
  Mesh *temporary_mesh = BKE_id_new<Mesh>(&bmain, proxy_name);
  session.proxy_mesh_session_uid = temporary_mesh->id.session_uid;
  temporary_mesh->id.tag |= ID_TAG_RUNTIME;
  BKE_mesh_nomain_to_mesh(BKE_mesh_copy_for_eval(*viewer_mesh), temporary_mesh, nullptr, false);
  temporary_mesh->id.tag |= ID_TAG_RUNTIME;

  snode.flag |= SNODE_PIN;
  scene.toolsettings->selectmode = select_mode;

  const float location[3] = {};
  const float rotation[3] = {};
  Object *temporary_object = ed::object::add_type_with_obdata(
      &C, OB_MESH, proxy_name, location, rotation, false, 0, &temporary_mesh->id);
  temporary_object->id.tag |= ID_TAG_RUNTIME;
  temporary_object->visibility_flag |= OB_HIDE_RENDER;
  if (editing) {
    /* Parenting an identity proxy preserves the complete evaluated matrix, including shear
     * inherited from a rotated, nonuniformly scaled parent. Decomposing that matrix loses shear.
     */
    temporary_object->parent = find_object(bmain, session.source_object_session_uid);
    temporary_object->partype = PAROBJECT;
    unit_m4(temporary_object->parentinv);
    BKE_object_apply_mat4(temporary_object, float4x4::identity().ptr(), false, false);
    DEG_relations_tag_update(&bmain);
  }
  else {
    BKE_object_apply_mat4(
        temporary_object, object_and_modifier->object->object_to_world().ptr(), false, false);
  }

  if (!ed::object::editmode_enter_ex(&bmain, &scene, temporary_object, 0)) {
    temporary_selection_object_delete(bmain, scene, *temporary_object);
    selection_context_restore(C, &snode, session);
    BKE_report(op.reports, RPT_ERROR, "Could not enter Edit Mode for the Viewer mesh");
    return OPERATOR_CANCELLED;
  }

  BMEditMesh *edit_mesh = BKE_editmesh_from_object(temporary_object);
  if (!edit_mesh) {
    temporary_selection_object_delete(bmain, scene, *temporary_object);
    selection_context_restore(C, &snode, session);
    BKE_report(op.reports, RPT_ERROR, "Could not create editable Viewer mesh data");
    return OPERATOR_CANCELLED;
  }

  EDBM_flag_disable_all(edit_mesh->bm, BM_ELEM_SELECT);
  EDBM_flag_disable_all(edit_mesh->bm, BM_ELEM_HIDDEN);
  EDBM_selectmode_set(edit_mesh, edit_mesh->bm, select_mode);
  session.proxy_signature = proxy_signature_get(*edit_mesh->bm);

  storage.selection_enabled = true;
  storage.editing_enabled = editing;
  storage.selection_object_session_uid = temporary_object->id.session_uid;
  selection_sessions().add_new(temporary_object->id.session_uid, std::move(session));

  WM_event_add_notifier(&C, NC_GEOM | ND_SELECT, temporary_mesh);
  WM_event_add_notifier(&C, NC_NODE | ND_DISPLAY, snode.edittree);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus geometry_viewer_selection_toggle_exec(bContext *C, wmOperator *op)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  bNode *viewer_node = viewer_node_from_operator(*C, *op);
  if (!viewer_node || !viewer_node->storage) {
    return OPERATOR_CANCELLED;
  }
  SpaceNode &snode = *CTX_wm_space_node(C);
  NodeGeometryViewer &storage = *static_cast<NodeGeometryViewer *>(viewer_node->storage);
  if (storage.selection_enabled) {
    selection_session_finish(*C, snode, *viewer_node);
    return OPERATOR_FINISHED;
  }
  return selection_enable(*C, *op, *viewer_node);
}

/* All transaction handles are identifiers, not pointers into topology caches or evaluation logs.
 */
static void edit_tree_tag(bContext &C, bNodeTree &tree)
{
  BKE_main_ensure_invariants(*CTX_data_main(&C), tree.id);
  DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(&C, NC_NODE | NA_EDITED, &tree);
}

static Vector<int> affected_points_get(BMesh &bm, const AttrDomain domain)
{
  BM_mesh_elem_index_ensure(&bm, BM_VERT | BM_EDGE | BM_FACE);
  Array<bool> points(bm.totvert, false);
  BMIter iter;
  if (domain == AttrDomain::Point) {
    return selected_indices_get(bm, domain);
  }
  if (domain == AttrDomain::Edge) {
    BMEdge *edge;
    BM_ITER_MESH (edge, &iter, &bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(edge, BM_ELEM_SELECT)) {
        points[BM_elem_index_get(edge->v1)] = true;
        points[BM_elem_index_get(edge->v2)] = true;
      }
    }
  }
  else {
    BMFace *face;
    BM_ITER_MESH (face, &iter, &bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(face, BM_ELEM_SELECT)) {
        BMLoop *loop = BM_FACE_FIRST_LOOP(face);
        do {
          points[BM_elem_index_get(loop->v)] = true;
          loop = loop->next;
        } while (loop != BM_FACE_FIRST_LOOP(face));
      }
    }
  }
  Vector<int> indices;
  for (const int index : points.index_range()) {
    if (points[index]) {
      indices.append(index);
    }
  }
  return indices;
}

static void edit_proxy_positions_update(bContext &C,
                                        Object &proxy,
                                        GeometryViewerSelectionSession &session)
{
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  BM_mesh_elem_table_ensure(em->bm, BM_VERT);
  for (const int index : session.positions.index_range()) {
    copy_v3_v3(BM_vert_at_index(em->bm, index)->co, session.positions[index]);
  }
  for (const int index : session.point_indices) {
    const float3 position = session.positions[index] + session.offset;
    copy_v3_v3(BM_vert_at_index(em->bm, index)->co, position);
  }
  EDBMUpdate_Params params{};
  params.calc_looptris = true;
  params.calc_normals = true;
  EDBM_update(id_cast<Mesh *>(proxy.data), &params);
  session.proxy_signature = proxy_signature_get(*em->bm);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, proxy.data);
}

static bool edit_position_matches(const float3 &a, const float3 &b)
{
  for (const int axis : IndexRange(3)) {
    const float tolerance = 2.0e-5f * std::max({1.0f, std::abs(a[axis]), std::abs(b[axis])});
    if (!std::isfinite(a[axis]) || !std::isfinite(b[axis]) ||
        std::abs(a[axis] - b[axis]) > tolerance)
    {
      return false;
    }
  }
  return true;
}

static Vector<float3> edit_result_positions(const GeometryViewerSelectionSession &session)
{
  Vector<float3> positions(session.positions);
  for (const int index : session.point_indices) {
    positions[index] = math::transform_point(session.local_transform, session.positions[index]);
  }
  return positions;
}

static bool edit_positions_match(const Span<float3> a, const Span<float3> b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (const int index : a.index_range()) {
    if (!edit_position_matches(a[index], b[index])) {
      return false;
    }
  }
  return true;
}

static bool edit_recipe_links_valid(const bNodeTree &tree, const Span<EditRecipeLink> links)
{
  for (const EditRecipeLink &expected : links) {
    int matches = 0;
    for (const bNodeLink &link : tree.links) {
      if (link.tonode->identifier == expected.to_node &&
          link.tosock->identifier == expected.to_socket)
      {
        /* Join Geometry has two intentional links. Reject extra/duplicate/muted inputs and
         * changes to their order, rather than treating any second input as a rewire. */
        const bool known = std::any_of(
            links.begin(), links.end(), [&](const EditRecipeLink &item) {
              return item.to_node == expected.to_node && item.to_socket == expected.to_socket &&
                     item.from_node == link.fromnode->identifier &&
                     item.from_socket == link.fromsock->identifier &&
                     (item.multi_input_sort_id < 0 ||
                      item.multi_input_sort_id == link.multi_input_sort_id);
            });
        if ((link.flag & NODE_LINK_MUTED) || !known) {
          return false;
        }
        matches += link.fromnode->identifier == expected.from_node &&
                   link.fromsock->identifier == expected.from_socket;
      }
    }
    if (matches != 1) {
      return false;
    }
  }
  return true;
}

static bool edit_move_batch_nodes_valid(const bNodeTree &tree,
                                        const GeometryViewerSelectionSession &session,
                                        const float3 &expected_offset)
{
  if (!session.move_batch) {
    return false;
  }
  tree.ensure_topology_cache();
  const GeometryViewerMoveBatch &batch = *session.move_batch;
  const bNode *capture = tree.node_by_id(batch.capture_node_identifier);
  const bNode *operation = tree.node_by_id(batch.operation_node_identifier);
  if (!capture || !operation || !capture->storage ||
      capture->idname != StringRef("GeometryNodeSelectByIndex") ||
      capture->custom2 != nodes::geometry_edit::SELECT_BY_INDEX_CAPTURED ||
      capture->custom1 != int(batch.domain) ||
      operation->idname != StringRef("GeometryNodeSetPosition") || (capture->flag & NODE_MUTED) ||
      (operation->flag & NODE_MUTED) || !edit_recipe_links_valid(tree, batch.links))
  {
    return false;
  }
  const auto &data = *static_cast<const NodeGeometryCapturedSelection *>(capture->storage);
  const bNodeSocket *offset = bke::node_find_socket(*operation, SOCK_IN, "Offset"_ustr);
  const bNodeSocket *position = bke::node_find_socket(*operation, SOCK_IN, "Position"_ustr);
  if (nodes::geometry_edit::capture_signature(data) != batch.capture_signature ||
      data.domain != int8_t(batch.domain) ||
      Span<int>(data.indices, data.indices_num) != batch.indices.as_span() ||
      Span<int>(data.point_indices, data.point_indices_num) != batch.point_indices.as_span() ||
      !offset || offset->is_directly_linked() || !position || position->is_directly_linked() ||
      float3(offset->default_value_typed<bNodeSocketValueVector>()->value) != expected_offset)
  {
    return false;
  }
  /* Do not silently change a newly shared branch or another consumer of the captured fields. */
  for (const bNodeLink &link : tree.links) {
    if ((link.fromnode == operation && link.tonode->identifier != session.viewer_identifier) ||
        (link.fromnode == capture && link.tonode != operation))
    {
      return false;
    }
  }
  return true;
}

static bool edit_move_batch_selection_matches(const BMEditMesh &em,
                                              const GeometryViewerMoveBatch &batch)
{
  const auto domain = selection_domain_from_mode(em.selectmode);
  return ELEM(em.selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE) && domain &&
         *domain == batch.domain &&
         selected_indices_get(*em.bm, *domain).as_span() == batch.indices.as_span() &&
         affected_points_get(*em.bm, *domain).as_span() == batch.point_indices.as_span();
}

static bool edit_session_validate(bContext &C,
                                  SpaceNode &snode,
                                  const bNode &viewer,
                                  GeometryViewerSelectionSession &session,
                                  const bool check_geometry)
{
  Main &bmain = *CTX_data_main(&C);
  Object *proxy = find_object(
      bmain,
      static_cast<const NodeGeometryViewer *>(viewer.storage)->selection_object_session_uid);
  const std::optional<ObjectAndModifier> context = get_geometry_nodes_modifier_for_node_editor(
      snode);
  bke::ComputeContextCache context_cache;
  const ComputeContext *compute_context = compute_context_for_edittree(snode, context_cache);
  if (!session.editing || !context || !compute_context ||
      compute_context->hash() != session.compute_context_hash ||
      context->object->id.session_uid != session.source_object_session_uid ||
      context->object->object_to_world() != session.source_to_world ||
      context->nmd->modifier.persistent_uid != session.modifier_uid ||
      snode.edittree->id.session_uid != session.tree_session_uid ||
      snode.edittree != context->nmd->node_group ||
      CTX_data_scene(&C)->id.session_uid != session.scene_session_uid ||
      CTX_data_scene(&C)->r.cfra != session.frame ||
      CTX_data_scene(&C)->r.subframe != session.subframe || !proxy ||
      !(proxy->mode & OB_MODE_EDIT))
  {
    return false;
  }
  bNodeLink *link = viewer_geometry_link(*snode.edittree, viewer);
  if (!link || link->tosock->identifier != session.viewer_socket_identifier ||
      link->fromnode->identifier !=
          (session.rotation_group_node_identifier ? session.rotation_group_node_identifier :
           session.duplicate_join_node_identifier ? session.duplicate_join_node_identifier :
           session.preview_node_identifier        ? session.preview_node_identifier :
           session.operation_node_identifier      ? session.operation_node_identifier :
                                                    session.source_node_identifier) ||
      (session.operation_node_identifier == 0 &&
       link->fromsock->identifier != session.source_socket_identifier))
  {
    return false;
  }
  if (session.operation_node_identifier) {
    snode.edittree->ensure_topology_cache();
    const bNode *operation = snode.edittree->node_by_id(session.operation_node_identifier);
    const bNode *capture = snode.edittree->node_by_id(session.capture_node_identifier);
    const bool scale_elements = ELEM(
        session.recipe, EditRecipe::UniformScale, EditRecipe::AxisScale);
    const char *operation_type =
        session.recipe == EditRecipe::LoopCut     ? "GeometryNodeLoopCutSlide" :
        session.recipe == EditRecipe::Extrude     ? "GeometryNodeExtrudeMesh" :
        session.recipe == EditRecipe::Delete      ? "GeometryNodeDeleteGeometry" :
        session.recipe == EditRecipe::Dissolve    ? "GeometryNodeDissolveElements" :
        session.recipe == EditRecipe::Merge       ? "GeometryNodeMergeByDistance" :
        session.recipe == EditRecipe::Triangulate ? "GeometryNodeTriangulate" :
        session.recipe == EditRecipe::FlipFaces   ? "GeometryNodeFlipFaces" :
        session.recipe == EditRecipe::ShadeSmooth ? "GeometryNodeSetShadeSmooth" :
        scale_elements                            ? "GeometryNodeScaleElements" :
                                                    "GeometryNodeSetPosition";
    if (!operation || !capture || operation->idname != StringRef(operation_type) ||
        capture->idname != StringRef("GeometryNodeSelectByIndex") ||
        capture->custom2 != nodes::geometry_edit::SELECT_BY_INDEX_CAPTURED ||
        !edit_recipe_links_valid(*snode.edittree, session.recipe_links))
    {
      return false;
    }
    if (session.reusing_move &&
        (session.recipe != EditRecipe::Offset ||
         !edit_move_batch_nodes_valid(*snode.edittree, session, session.offset)))
    {
      return false;
    }
    for (const int identifier : session.helper_node_identifiers) {
      if (!snode.edittree->node_by_id(identifier)) {
        return false;
      }
    }
    if (session.rotation_group_node_identifier) {
      const bNode *group = snode.edittree->node_by_id(session.rotation_group_node_identifier);
      const bNodeSocket *rotation =
          group ?
              bke::node_find_socket(*group, SOCK_IN, UString(session.rotation_socket_identifier)) :
              nullptr;
      if (!group || group->idname != StringRef("GeometryNodeGroup") ||
          id_cast<const bNodeTree *>(group->id) !=
              find_tree(bmain, session.rotation_group_tree_uid) ||
          !rotation || rotation->is_directly_linked() ||
          float3(rotation->default_value_typed<bNodeSocketValueRotation>()->value_euler) !=
              session.rotation_euler)
      {
        return false;
      }
    }
    if (ELEM(session.recipe, EditRecipe::Merge, EditRecipe::Duplicate) ||
        edit_recipe_is_face_tool(session.recipe))
    {
      const auto &data = *static_cast<const NodeGeometryCapturedSelection *>(capture->storage);
      if (nodes::geometry_edit::capture_signature(data) != session.source_signature ||
          data.domain != int8_t(session.edit_domain) ||
          Span<int>(data.indices, data.indices_num) != session.capture_indices.as_span() ||
          Span<int>(data.point_indices, data.point_indices_num) !=
              session.capture_point_indices.as_span())
      {
        return false;
      }
      if (session.recipe == EditRecipe::Merge) {
        const bNodeSocket *mode = bke::node_find_socket(*operation, SOCK_IN, "Mode"_ustr);
        const bNodeSocket *distance = bke::node_find_socket(*operation, SOCK_IN, "Distance"_ustr);
        if (!mode || !distance || mode->is_directly_linked() || distance->is_directly_linked() ||
            mode->default_value_typed<bNodeSocketValueMenu>()->value != int(session.merge_mode) ||
            distance->default_value_typed<bNodeSocketValueFloat>()->value !=
                session.merge_distance)
        {
          return false;
        }
      }
      else if (session.recipe == EditRecipe::Duplicate) {
        if (session.helper_node_identifiers.size() != 3 || !session.preview_node_identifier ||
            !session.duplicate_join_node_identifier)
        {
          return false;
        }
        const bNode *separate = snode.edittree->node_by_id(session.helper_node_identifiers[0]);
        const bNode *join = snode.edittree->node_by_id(session.duplicate_join_node_identifier);
        const bNodeSocket *selection = bke::node_find_socket(
            *operation, SOCK_IN, "Selection"_ustr);
        const bNodeSocket *offset = bke::node_find_socket(*operation, SOCK_IN, "Offset"_ustr);
        if (!separate || !separate->storage ||
            separate->idname != StringRef("GeometryNodeSeparateGeometry") ||
            static_cast<const NodeGeometrySeparateGeometry *>(separate->storage)->domain !=
                int8_t(session.edit_domain) ||
            !join || join->idname != StringRef("GeometryNodeJoinGeometry") || !selection ||
            selection->is_directly_linked() ||
            !selection->default_value_typed<bNodeSocketValueBoolean>()->value || !offset ||
            offset->is_directly_linked() ||
            float3(offset->default_value_typed<bNodeSocketValueVector>()->value) != session.offset)
        {
          return false;
        }
      }
      else if (session.recipe == EditRecipe::Triangulate) {
        const bNodeSocket *quad = bke::node_find_socket(*operation, SOCK_IN, "Quad Method"_ustr);
        const bNodeSocket *ngon = bke::node_find_socket(*operation, SOCK_IN, "N-gon Method"_ustr);
        if (!quad || !ngon || quad->is_directly_linked() || ngon->is_directly_linked() ||
            quad->default_value_typed<bNodeSocketValueMenu>()->value !=
                session.triangulate_quad_method ||
            ngon->default_value_typed<bNodeSocketValueMenu>()->value !=
                session.triangulate_ngon_method ||
            session.helper_node_identifiers.size() != 2)
        {
          return false;
        }
        const bNode *index = snode.edittree->node_by_id(session.helper_node_identifiers[0]);
        const bNode *tag = snode.edittree->node_by_id(session.helper_node_identifiers[1]);
        const bNodeSocket *name = bke::node_find_socket(*tag, SOCK_IN, "Name"_ustr);
        const bNodeSocket *selection = bke::node_find_socket(*tag, SOCK_IN, "Selection"_ustr);
        if (index->idname != StringRef("GeometryNodeInputIndex") ||
            tag->idname != StringRef("GeometryNodeStoreNamedAttribute") || !tag->storage ||
            static_cast<const NodeGeometryStoreNamedAttribute *>(tag->storage)->domain !=
                int8_t(AttrDomain::Face) ||
            static_cast<const NodeGeometryStoreNamedAttribute *>(tag->storage)->data_type !=
                CD_PROP_INT32 ||
            !name || name->is_directly_linked() ||
            StringRef(name->default_value_typed<bNodeSocketValueString>()->value) !=
                session.top_attribute_name ||
            !selection || selection->is_directly_linked() ||
            !selection->default_value_typed<bNodeSocketValueBoolean>()->value)
        {
          return false;
        }
      }
      else if (session.recipe == EditRecipe::ShadeSmooth) {
        const bNodeSocket *smooth = bke::node_find_socket(
            *operation, SOCK_IN, "Shade Smooth"_ustr);
        if (operation->custom1 != int(AttrDomain::Face) || !smooth ||
            smooth->is_directly_linked() ||
            bool(smooth->default_value_typed<bNodeSocketValueBoolean>()->value) !=
                session.face_smooth)
        {
          return false;
        }
      }
    }
    if (session.recipe == EditRecipe::LoopCut) {
      const bNodeSocket *cuts = bke::node_find_socket(*operation, SOCK_IN, "Cuts"_ustr);
      const bNodeSocket *factor = bke::node_find_socket(*operation, SOCK_IN, "Factor"_ustr);
      const auto &data = *static_cast<const NodeGeometryCapturedSelection *>(capture->storage);
      if (!cuts || !factor || cuts->is_directly_linked() || factor->is_directly_linked() ||
          cuts->default_value_typed<bNodeSocketValueInt>()->value != session.loop_cut_cuts ||
          factor->default_value_typed<bNodeSocketValueFloat>()->value != session.loop_cut_factor ||
          nodes::geometry_edit::capture_signature(data) != session.source_signature ||
          data.domain != int8_t(AttrDomain::Edge) ||
          Span<int>(data.indices, data.indices_num) != session.picked_indices.as_span() ||
          Span<int>(data.point_indices, data.point_indices_num) != session.point_indices.as_span())
      {
        return false;
      }
    }
    if (ELEM(session.recipe, EditRecipe::Delete, EditRecipe::Dissolve)) {
      const auto &data = *static_cast<const NodeGeometryCapturedSelection *>(capture->storage);
      if (nodes::geometry_edit::capture_signature(data) != session.source_signature ||
          data.domain != int8_t(session.edit_domain) ||
          Span<int>(data.indices, data.indices_num) != session.picked_indices.as_span() ||
          Span<int>(data.point_indices, data.point_indices_num) != session.point_indices.as_span())
      {
        return false;
      }
      if (session.recipe == EditRecipe::Delete) {
        const auto &storage = *static_cast<const NodeGeometryDeleteGeometry *>(operation->storage);
        if (storage.domain != int(session.edit_domain) || storage.mode != session.delete_mode) {
          return false;
        }
      }
      else if (operation->custom1 != int(session.edit_domain)) {
        return false;
      }
    }
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  if (!em) {
    return false;
  }
  const auto proxy_signature = proxy_signature_get(*em->bm);
  if (session.native_operator && session.recipe != EditRecipe::LoopCut) {
    /* Positions belong to TransInfo until finish. Only topology is invariant during the gesture.
     */
    if (proxy_signature.topology_hash != session.proxy_signature.topology_hash ||
        em->bm->totvert != session.verts_num || em->bm->totedge != session.edges_num ||
        em->bm->totface != session.faces_num)
    {
      return false;
    }
    if (em->selectmode != selection_mode_from_domain(session.edit_domain)) {
      return false;
    }
  }
  else if (proxy_signature != session.proxy_signature) {
    return false;
  }
  if (session.recipe == EditRecipe::LoopCut) {
    if (!ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
      return false;
    }
    for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
      if (!selected_indices_get(*em->bm, domain, BM_ELEM_HIDDEN).is_empty()) {
        return false;
      }
    }
  }
  if (check_geometry) {
    const bke::GeometrySet *geometry = viewer_geometry_get(snode, viewer);
    if (!geometry || geometry->has_instances() || geometry->has_curves() ||
        geometry->has_pointcloud() || geometry->has_volume() || geometry->has_grease_pencil())
    {
      return false;
    }
    const Mesh *mesh = geometry->get_mesh();
    if (session.recipe == EditRecipe::LoopCut) {
      /* The custom cut tool never writes or replaces the original picking BMesh. Only its
       * evaluated node result changes until commit, when a new proxy is created after undo. */
      if (!mesh ||
          nodes::geometry_edit::mesh_signature(*mesh) != session.loop_cut_preview_signature)
      {
        return false;
      }
      auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(snode);
      if (auto *log = logs.get_main_tree_log(viewer)) {
        log->ensure_node_warnings(bmain);
        for (const int identifier :
             {session.capture_node_identifier, session.operation_node_identifier})
        {
          if (const auto *node_log = log->find_node_log(identifier)) {
            if (!node_log->warnings.is_empty()) {
              return false;
            }
          }
        }
      }
      return true;
    }
    if (session.native_operator ||
        ELEM(session.recipe, EditRecipe::Extrude, EditRecipe::Duplicate))
    {
      if (!mesh ||
          nodes::geometry_edit::mesh_signature(*mesh).topology_hash !=
              session.proxy_signature.topology_hash ||
          !edit_positions_match(mesh->vert_positions(), edit_result_positions(session)))
      {
        return false;
      }
      /* Capture validation detects upstream changes even when a collapsed scale could conceal
       * them in the result. Warnings are logged by the capture at its original geometry stage. */
      auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(snode);
      if (auto *log = logs.get_main_tree_log(viewer)) {
        log->ensure_node_warnings(bmain);
        if (const auto *capture_log = log->find_node_log(session.capture_node_identifier)) {
          if (!capture_log->warnings.is_empty()) {
            return false;
          }
        }
      }
      return true;
    }
    auto expected = session.source_signature;
    expected.position_hash = session.proxy_signature.position_hash;
    if (!mesh || nodes::geometry_edit::mesh_signature(*mesh) != expected) {
      return false;
    }
  }
  return true;
}

/** Place only this gesture's nodes, using absolute coordinates even inside a frame. Keeping the
 * origin fixed lets a modal recipe change replace its helpers without drifting to the right. */
static void edit_recipe_layout(bNodeTree &tree,
                               bNode &viewer,
                               GeometryViewerSelectionSession &session)
{
  tree.ensure_topology_cache();
  const bNode *source = tree.node_by_id(session.source_node_identifier);
  bNode *capture = tree.node_by_id(session.capture_node_identifier);
  bNode *operation = tree.node_by_id(session.operation_node_identifier);
  if (!source || !capture || !operation) {
    return;
  }
  constexpr float spacing = 80;
  if (!session.viewer_original_location) {
    session.viewer_original_location = float2(viewer.location);
    session.viewer_layout_location = float2(viewer.location);
    session.recipe_layout_origin = float2(
        std::max(source->location[0] + source->width + spacing, viewer.location[0]),
        source->location[1]);
  }
  float2 cursor = session.recipe_layout_origin;
  const auto place = [&](bNode &node) {
    copy_v2_v2(node.location, cursor);
    cursor.x += node.width + spacing;
  };
  place(*capture);
  for (const int identifier : session.helper_node_identifiers) {
    if (identifier != session.preview_node_identifier &&
        identifier != session.duplicate_join_node_identifier)
    {
      if (bNode *helper = tree.node_by_id(identifier)) {
        place(*helper);
      }
    }
  }
  place(*operation);
  if (bNode *preview = tree.node_by_id(session.preview_node_identifier)) {
    place(*preview);
  }
  if (bNode *join = tree.node_by_id(session.duplicate_join_node_identifier)) {
    place(*join);
  }
  /* Respect a manual Viewer move made while a transaction is pending. */
  if (float2(viewer.location) == session.viewer_layout_location) {
    copy_v2_v2(viewer.location, cursor);
    session.viewer_layout_location = cursor;
  }
}

static void edit_rotation_group_discard(bContext &C, GeometryViewerSelectionSession &session)
{
  if (bNodeTree *group = find_tree(*CTX_data_main(&C), session.rotation_group_tree_uid)) {
    /* Only discard this transaction's unreferenced group. Never delete a group reused elsewhere.
     */
    if (group->id.us == 0) {
      BKE_id_delete(CTX_data_main(&C), &group->id);
    }
  }
  session.rotation_group_tree_uid = 0;
  session.rotation_group_node_identifier = 0;
  session.rotation_socket_identifier.clear();
  session.rotation_group_preparing = false;
}

static void edit_transaction_rollback(bContext &C, GeometryViewerSelectionSession &session)
{
  if (session.reusing_move) {
    bNodeTree *tree = find_tree(*CTX_data_main(&C), session.tree_session_uid);
    bool restored = false;
    if (tree && session.move_batch) {
      tree->ensure_topology_cache();
      bNode *operation = tree->node_by_id(session.move_batch->operation_node_identifier);
      bNodeSocket *offset = operation ? bke::node_find_socket(*operation, SOCK_IN, "Offset"_ustr) :
                                        nullptr;
      /* Restore only the value written by this gesture, not an external property/link edit. */
      if (operation && operation->idname == StringRef("GeometryNodeSetPosition") && offset &&
          !offset->is_directly_linked() &&
          float3(offset->default_value_typed<bNodeSocketValueVector>()->value) == session.offset)
      {
        copy_v3_v3(offset->default_value_typed<bNodeSocketValueVector>()->value,
                   session.move_batch->offset);
        BKE_ntree_update_tag_node_property(tree, operation);
        edit_tree_tag(C, *tree);
        restored = true;
      }
    }
    if (!restored) {
      session.move_batch.reset();
    }
    session.reusing_move = false;
    session.operation_node_identifier = session.capture_node_identifier = 0;
    session.helper_node_identifiers.clear();
    session.recipe_links.clear();
    session.viewer_original_location.reset();
    session.recipe = EditRecipe::Offset;
    session.local_transform = float4x4::identity();
    session.transform_kind = GeometryViewerTransform::Move;
    session.rotation_euler = float3(0);
    session.offset = float3(0);
    session.point_indices.clear();
    session.picked_indices.clear();
    session.capture_indices.clear();
    session.capture_point_indices.clear();
    return;
  }
  if (!session.operation_node_identifier && !session.capture_node_identifier) {
    return;
  }
  bNodeTree *tree = find_tree(*CTX_data_main(&C), session.tree_session_uid);
  if (!tree) {
    session.viewer_original_location.reset();
    session.operation_node_identifier = session.capture_node_identifier = 0;
    session.preview_node_identifier = 0;
    session.extrusion_armed = session.extrusion_preparing = false;
    session.duplication_armed = session.duplication_preparing = session.merge_preparing = false;
    session.duplicate_join_node_identifier = 0;
    session.loop_cut_preparing = session.loop_cut_sliding = false;
    session.deletion_preparing = false;
    session.face_tool_preparing = false;
    session.helper_node_identifiers.clear();
    session.recipe_links.clear();
    edit_rotation_group_discard(C, session);
    return;
  }
  /* Remove only nodes owned by this transaction. An externally rewired Viewer is not overwritten.
   */
  Vector<int> owned_nodes(session.helper_node_identifiers);
  owned_nodes.append(session.operation_node_identifier);
  owned_nodes.append(session.capture_node_identifier);
  for (const int identifier : owned_nodes) {
    tree->ensure_topology_cache();
    if (bNode *node = tree->node_by_id(identifier)) {
      bke::node_remove_node(CTX_data_main(&C), *tree, *node, true);
    }
  }
  edit_rotation_group_discard(C, session);
  tree->ensure_topology_cache();
  bNode *viewer = tree->node_by_id(session.viewer_identifier);
  if (viewer && session.viewer_original_location &&
      float2(viewer->location) == session.viewer_layout_location)
  {
    copy_v2_v2(viewer->location, *session.viewer_original_location);
  }
  session.viewer_original_location.reset();
  bNode *source = tree->node_by_id(session.source_node_identifier);
  bNodeSocket *to = viewer ? bke::node_find_socket(
                                 *viewer, SOCK_IN, UString(session.viewer_socket_identifier)) :
                             nullptr;
  bNodeSocket *from = source ? bke::node_find_socket(
                                   *source, SOCK_OUT, UString(session.source_socket_identifier)) :
                               nullptr;
  bool has_link = false;
  for (const bNodeLink &link : tree->links) {
    has_link |= link.tosock == to;
  }
  if (to && from && !has_link) {
    bNodeLink &link = bke::node_add_link(*tree, *source, *from, *viewer, *to);
    link.flag = session.original_link_flag;
  }
  node_deselect_all(*tree);
  tree->ensure_topology_cache();
  for (const int identifier : session.selected_node_identifiers) {
    if (bNode *node = tree->node_by_id(identifier)) {
      bke::node_set_selected(*node, true);
    }
  }
  if (bNode *active = tree->node_by_id(session.active_node_identifier)) {
    bke::node_set_active(*tree, *active);
  }
  session.operation_node_identifier = session.capture_node_identifier = 0;
  session.preview_node_identifier = 0;
  session.extrusion_armed = session.extrusion_preparing = false;
  session.duplication_armed = session.duplication_preparing = session.merge_preparing = false;
  session.duplicate_join_node_identifier = 0;
  session.loop_cut_preparing = session.loop_cut_sliding = false;
  session.deletion_preparing = false;
  session.face_tool_preparing = false;
  session.top_attribute_name.clear();
  session.helper_node_identifiers.clear();
  session.recipe_links.clear();
  session.recipe = EditRecipe::Offset;
  session.local_transform = float4x4::identity();
  session.transform_kind = GeometryViewerTransform::Move;
  session.rotation_euler = float3(0);
  session.offset = float3(0);
  session.point_indices.clear();
  session.picked_indices.clear();
  session.capture_indices.clear();
  session.capture_point_indices.clear();
  edit_tree_tag(C, *tree);
}

static bool edit_transaction_begin(bContext &C,
                                   bNodeTree &tree,
                                   bNode &viewer,
                                   Object &proxy,
                                   GeometryViewerSelectionSession &session,
                                   const std::optional<int> seed_edge = std::nullopt,
                                   const std::optional<AttrDomain> capture_domain = std::nullopt)
{
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  const auto domain = seed_edge      ? std::optional(AttrDomain::Edge) :
                      capture_domain ? capture_domain :
                                       selection_domain_from_mode(em->selectmode);
  if (!domain) {
    return false;
  }
  session.edit_domain = *domain;
  BM_mesh_elem_index_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);
  BM_mesh_elem_table_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);
  if (seed_edge) {
    if (*seed_edge < 0 || *seed_edge >= em->bm->totedge) {
      return false;
    }
    const BMEdge *edge = BM_edge_at_index(em->bm, *seed_edge);
    session.picked_indices = {*seed_edge};
    session.point_indices = {BM_elem_index_get(edge->v1), BM_elem_index_get(edge->v2)};
    std::sort(session.point_indices.begin(), session.point_indices.end());
  }
  else {
    session.picked_indices = selected_indices_get(*em->bm, *domain);
    session.point_indices = affected_points_get(*em->bm, *domain);
  }
  if (session.picked_indices.is_empty()) {
    return false;
  }
  session.move_batch.reset();
  session.capture_indices = session.picked_indices;
  session.capture_point_indices = session.point_indices;
  session.active_element_type = 0;
  session.active_element_index = -1;
  BMEditSelection active{};
  if (BM_select_history_active_get(em->bm, &active)) {
    session.active_element_type = active.htype;
    session.active_element_index = BM_elem_index_get(active.ele);
  }
  else if (*domain == AttrDomain::Face) {
    if (BMFace *face = BM_mesh_active_face_get(em->bm, false, true)) {
      session.active_element_type = BM_FACE;
      session.active_element_index = BM_elem_index_get(face);
    }
  }
  session.pivot = float3(0);
  for (const int index : session.point_indices) {
    session.pivot += session.positions[index];
  }
  if (!session.point_indices.is_empty()) {
    session.pivot /= session.point_indices.size();
  }
  session.selected_node_identifiers.clear();
  for (const bNode &node : tree.nodes) {
    if (node.flag & NODE_SELECT) {
      session.selected_node_identifiers.append(node.identifier);
    }
  }
  if (const bNode *active_node = bke::node_get_active(tree)) {
    session.active_node_identifier = active_node->identifier;
  }
  bNodeLink *original = viewer_geometry_link(tree, viewer);
  bNode *source = original->fromnode;
  bNodeSocket *source_socket = original->fromsock;
  bNodeSocket *viewer_socket = original->tosock;
  bNode *capture = bke::node_add_node(&C, tree, "GeometryNodeSelectByIndex"_ustr);
  if (!capture) {
    return false;
  }
  session.capture_node_identifier = capture->identifier;
  capture->custom1 = int16_t(*domain);
  capture->custom2 = nodes::geometry_edit::SELECT_BY_INDEX_CAPTURED;
  nodes::geometry_edit::capture_initialize(
      *static_cast<NodeGeometryCapturedSelection *>(capture->storage),
      session.source_signature,
      *domain,
      session.picked_indices,
      session.point_indices);
  BKE_ntree_update_tag_node_property(&tree, capture);
  BKE_main_ensure_invariants(*CTX_data_main(&C), tree.id);
  capture_indices_text_update(*capture);
  bNode *operation = bke::node_add_node(&C, tree, "GeometryNodeSetPosition"_ustr);
  if (!operation) {
    edit_transaction_rollback(C, session);
    return false;
  }
  session.operation_node_identifier = operation->identifier;
  session.recipe = EditRecipe::Offset;
  capture->parent = operation->parent = viewer.parent;
  /* List lookup is safe immediately after node insertion, before topology caches are rebuilt. */
  bNodeSocket *capture_geometry = bke::node_find_socket(*capture, SOCK_IN, "Geometry"_ustr);
  bNodeSocket *capture_points = bke::node_find_socket(*capture, SOCK_OUT, "Points"_ustr);
  bNodeSocket *geometry = bke::node_find_socket(*operation, SOCK_IN, "Geometry"_ustr);
  bNodeSocket *selection = bke::node_find_socket(*operation, SOCK_IN, "Selection"_ustr);
  bNodeSocket *result = bke::node_find_socket(*operation, SOCK_OUT, "Geometry"_ustr);
  if (!capture_geometry || !capture_points || !geometry || !selection || !result) {
    edit_transaction_rollback(C, session);
    return false;
  }
  bke::node_remove_link(&tree, *original);
  bke::node_add_link(tree, *source, *source_socket, *capture, *capture_geometry);
  bke::node_add_link(tree, *source, *source_socket, *operation, *geometry);
  bke::node_add_link(tree, *capture, *capture_points, *operation, *selection);
  bke::node_add_link(tree, *operation, *result, viewer, *viewer_socket);
  session.recipe_links = {
      {session.source_node_identifier,
       session.source_socket_identifier,
       capture->identifier,
       "Geometry"},
      {session.source_node_identifier,
       session.source_socket_identifier,
       operation->identifier,
       "Geometry"},
      {capture->identifier, "Points", operation->identifier, "Selection"},
      {operation->identifier, "Geometry", viewer.identifier, session.viewer_socket_identifier}};
  session.generation++;
  edit_recipe_layout(tree, viewer, session);
  edit_tree_tag(C, tree);
  return true;
}

static bool edit_move_batch_begin(bNodeTree &tree,
                                  Object &proxy,
                                  GeometryViewerSelectionSession &session)
{
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  if (!session.move_batch ||
      session.source_node_identifier != session.move_batch->operation_node_identifier ||
      session.source_socket_identifier != "Geometry" ||
      !edit_move_batch_selection_matches(*em, *session.move_batch) ||
      !edit_move_batch_nodes_valid(tree, session, session.move_batch->offset))
  {
    return false;
  }
  const GeometryViewerMoveBatch &batch = *session.move_batch;
  session.reusing_move = true;
  session.capture_node_identifier = batch.capture_node_identifier;
  session.operation_node_identifier = batch.operation_node_identifier;
  session.edit_domain = batch.domain;
  session.picked_indices = session.capture_indices = batch.indices;
  session.point_indices = session.capture_point_indices = batch.point_indices;
  session.recipe_links = batch.links;
  session.offset = batch.offset;
  session.local_transform = float4x4::identity();
  session.recipe = EditRecipe::Offset;
  session.active_element_type = 0;
  session.active_element_index = -1;
  BMEditSelection active{};
  if (BM_select_history_active_get(em->bm, &active)) {
    session.active_element_type = active.htype;
    session.active_element_index = BM_elem_index_get(active.ele);
  }
  else if (batch.domain == AttrDomain::Face) {
    if (BMFace *face = BM_mesh_active_face_get(em->bm, false, true)) {
      session.active_element_type = BM_FACE;
      session.active_element_index = BM_elem_index_get(face);
    }
  }
  return true;
}

static bool edit_link_add(bNodeTree &tree,
                          GeometryViewerSelectionSession &session,
                          const int from_identifier,
                          const std::string &from_name,
                          const int to_identifier,
                          const std::string &to_name)
{
  tree.ensure_topology_cache();
  bNode *from = tree.node_by_id(from_identifier);
  bNode *to = tree.node_by_id(to_identifier);
  bNodeSocket *output = from ? bke::node_find_socket(*from, SOCK_OUT, UString(from_name)) :
                               nullptr;
  bNodeSocket *input = to ? bke::node_find_socket(*to, SOCK_IN, UString(to_name)) : nullptr;
  if (!output || !input) {
    return false;
  }
  Vector<bNodeLink *> old_links;
  for (bNodeLink &link : tree.links) {
    if (link.tosock == input && !input->is_multi_input()) {
      old_links.append(&link);
    }
  }
  for (bNodeLink *link : old_links) {
    bke::node_remove_link(&tree, *link);
  }
  const bNodeLink &link = bke::node_add_link(tree, *from, *output, *to, *input);
  session.recipe_links.append({from_identifier,
                               from_name,
                               to_identifier,
                               to_name,
                               input->is_multi_input() ? link.multi_input_sort_id : -1});
  return true;
}

static bool edit_recipe_create(bContext &C,
                               bNodeTree &tree,
                               bNode &viewer,
                               GeometryViewerSelectionSession &session,
                               const EditRecipe recipe)
{
  if (recipe == session.recipe && session.operation_node_identifier) {
    return true;
  }
  /* A modal mode/constraint change can need a different recipe. Preserve the original capture,
   * and remove only this transaction's operation/helpers, never its source or another branch. */
  Vector<int> remove_nodes(session.helper_node_identifiers);
  remove_nodes.append(session.operation_node_identifier);
  for (const int identifier : remove_nodes) {
    tree.ensure_topology_cache();
    if (bNode *node = tree.node_by_id(identifier)) {
      bke::node_remove_node(CTX_data_main(&C), tree, *node, true);
    }
  }
  session.helper_node_identifiers.clear();
  session.recipe_links.clear();
  session.operation_node_identifier = 0;
  session.preview_node_identifier = 0;
  session.duplicate_join_node_identifier = 0;
  session.recipe = recipe;
  const bool scale_elements = ELEM(recipe, EditRecipe::UniformScale, EditRecipe::AxisScale);
  const bool extrude = recipe == EditRecipe::Extrude;
  const bool loop_cut = recipe == EditRecipe::LoopCut;
  const bool deleting = recipe == EditRecipe::Delete;
  const bool dissolving = recipe == EditRecipe::Dissolve;
  const bool merging = recipe == EditRecipe::Merge;
  const bool duplicating = recipe == EditRecipe::Duplicate;
  const bool triangulating = recipe == EditRecipe::Triangulate;
  const bool flipping = recipe == EditRecipe::FlipFaces;
  const bool shading = recipe == EditRecipe::ShadeSmooth;
  const bool mesh_operation = extrude || loop_cut || dissolving || triangulating || flipping;
  bNode *operation = bke::node_add_node(&C,
                                        tree,
                                        loop_cut       ? "GeometryNodeLoopCutSlide"_ustr :
                                        extrude        ? "GeometryNodeExtrudeMesh"_ustr :
                                        deleting       ? "GeometryNodeDeleteGeometry"_ustr :
                                        dissolving     ? "GeometryNodeDissolveElements"_ustr :
                                        merging        ? "GeometryNodeMergeByDistance"_ustr :
                                        triangulating  ? "GeometryNodeTriangulate"_ustr :
                                        flipping       ? "GeometryNodeFlipFaces"_ustr :
                                        shading        ? "GeometryNodeSetShadeSmooth"_ustr :
                                        scale_elements ? "GeometryNodeScaleElements"_ustr :
                                                         "GeometryNodeSetPosition"_ustr);
  if (!operation) {
    return false;
  }
  session.operation_node_identifier = operation->identifier;
  operation->parent = viewer.parent;
  if (triangulating) {
    bke::node_find_socket(*operation, SOCK_IN, "Quad Method"_ustr)
        ->default_value_typed<bNodeSocketValueMenu>()
        ->value = session.triangulate_quad_method;
    bke::node_find_socket(*operation, SOCK_IN, "N-gon Method"_ustr)
        ->default_value_typed<bNodeSocketValueMenu>()
        ->value = session.triangulate_ngon_method;
    BKE_ntree_update_tag_node_property(&tree, operation);
  }
  if (shading) {
    operation->custom1 = int16_t(AttrDomain::Face);
    bke::node_find_socket(*operation, SOCK_IN, "Shade Smooth"_ustr)
        ->default_value_typed<bNodeSocketValueBoolean>()
        ->value = session.face_smooth;
    BKE_ntree_update_tag_node_property(&tree, operation);
  }
  if (merging) {
    bke::node_find_socket(*operation, SOCK_IN, "Mode"_ustr)
        ->default_value_typed<bNodeSocketValueMenu>()
        ->value = int(session.merge_mode);
    bke::node_find_socket(*operation, SOCK_IN, "Distance"_ustr)
        ->default_value_typed<bNodeSocketValueFloat>()
        ->value = session.merge_distance;
    BKE_ntree_update_tag_node_property(&tree, operation);
  }
  if (scale_elements || dissolving) {
    operation->custom1 = int16_t(session.edit_domain);
  }
  if (deleting) {
    auto &storage = *static_cast<NodeGeometryDeleteGeometry *>(operation->storage);
    storage.domain = int(session.edit_domain);
    storage.mode = session.delete_mode;
    BKE_ntree_update_tag_node_property(&tree, operation);
  }
  if (extrude) {
    auto &storage = *static_cast<NodeGeometryExtrudeMesh *>(operation->storage);
    storage.mode = session.edit_domain == AttrDomain::Point ? GEO_NODE_EXTRUDE_MESH_VERTICES :
                   session.edit_domain == AttrDomain::Edge  ? GEO_NODE_EXTRUDE_MESH_EDGES :
                                                              GEO_NODE_EXTRUDE_MESH_FACES;
    bke::node_find_socket(*operation, SOCK_IN, "Individual"_ustr)
        ->default_value_typed<bNodeSocketValueBoolean>()
        ->value = session.extrude_individual;
    BKE_ntree_update_tag_node_property(&tree, operation);
  }
  if (!edit_link_add(tree,
                     session,
                     session.source_node_identifier,
                     session.source_socket_identifier,
                     session.capture_node_identifier,
                     "Geometry"))
  {
    return false;
  }
  tree.ensure_topology_cache();
  bNode *capture = tree.node_by_id(session.capture_node_identifier);
  if (duplicating) {
    /* Duplicate Elements isolates faces and edges. A selected Separate Geometry stream keeps
     * shared vertices/edges within the region and Join Geometry preserves the untouched source.
     * Selection is evaluated only on the captured input, never on the reordered copy. */
    bNode *separate = bke::node_add_node(&C, tree, "GeometryNodeSeparateGeometry"_ustr);
    if (!separate) {
      return false;
    }
    session.helper_node_identifiers.append(separate->identifier);
    static_cast<NodeGeometrySeparateGeometry *>(separate->storage)->domain = int8_t(
        session.edit_domain);
    STRNCPY_UTF8(separate->label, "Connected Duplicate");
    separate->parent = viewer.parent;
    BKE_ntree_update_tag_node_property(&tree, separate);
    bNode *join = bke::node_add_node(&C, tree, "GeometryNodeJoinGeometry"_ustr);
    if (!join) {
      return false;
    }
    session.helper_node_identifiers.append(join->identifier);
    session.duplicate_join_node_identifier = join->identifier;
    join->parent = viewer.parent;
    bNode *picking = bke::node_add_node(&C, tree, "GeometryNodeStoreNamedAttribute"_ustr);
    if (!picking) {
      return false;
    }
    session.helper_node_identifiers.append(picking->identifier);
    session.preview_node_identifier = picking->identifier;
    auto &storage = *static_cast<NodeGeometryStoreNamedAttribute *>(picking->storage);
    storage.data_type = CD_PROP_BOOL;
    storage.domain = int8_t(session.edit_domain);
    STRNCPY_UTF8(picking->label, "Temporary Viewer Picking");
    picking->parent = viewer.parent;
    BKE_ntree_update_tag_node_property(&tree, picking);
    BKE_main_ensure_invariants(*CTX_data_main(&C), tree.id);
    bke::node_find_socket(*picking, SOCK_IN, "Value"_ustr)
        ->default_value_typed<bNodeSocketValueBoolean>()
        ->value = true;
    auto &name = bke::node_find_socket(*picking, SOCK_IN, "Name"_ustr)
                     ->default_value_typed<bNodeSocketValueString>()
                     ->value;
    STRNCPY_UTF8(name, session.top_attribute_name.c_str());
    STRNCPY_UTF8(operation->label, "Duplicate Offset");
    if (!edit_link_add(tree,
                       session,
                       session.source_node_identifier,
                       session.source_socket_identifier,
                       separate->identifier,
                       "Geometry") ||
        !edit_link_add(
            tree, session, capture->identifier, "Selection", separate->identifier, "Selection") ||
        !edit_link_add(
            tree, session, separate->identifier, "Selection", operation->identifier, "Geometry") ||
        !edit_link_add(
            tree, session, operation->identifier, "Geometry", picking->identifier, "Geometry") ||
        !edit_link_add(
            tree, session, picking->identifier, "Geometry", join->identifier, "Geometry") ||
        !edit_link_add(tree,
                       session,
                       session.source_node_identifier,
                       session.source_socket_identifier,
                       join->identifier,
                       "Geometry") ||
        !edit_link_add(tree,
                       session,
                       join->identifier,
                       "Geometry",
                       viewer.identifier,
                       session.viewer_socket_identifier))
    {
      return false;
    }
    edit_recipe_layout(tree, viewer, session);
    return true;
  }
  if (!edit_link_add(tree,
                     session,
                     session.source_node_identifier,
                     session.source_socket_identifier,
                     operation->identifier,
                     mesh_operation ? "Mesh" : "Geometry") ||
      !edit_link_add(tree,
                     session,
                     capture->identifier,
                     (scale_elements || mesh_operation || deleting || shading) ? "Selection" :
                                                                                 "Points",
                     operation->identifier,
                     "Selection") ||
      !edit_link_add(tree,
                     session,
                     operation->identifier,
                     mesh_operation ? "Mesh" : "Geometry",
                     viewer.identifier,
                     session.viewer_socket_identifier))
  {
    return false;
  }
  if (triangulating) {
    /* Carry the source face index through the existing kernel's face-attribute propagation.
     * Both helpers are transaction-local, not part of the committed graph or saved geometry. */
    bNode *index = bke::node_add_node(&C, tree, "GeometryNodeInputIndex"_ustr);
    if (!index) {
      return false;
    }
    session.helper_node_identifiers.append(index->identifier);
    index->parent = viewer.parent;
    bNode *tag = bke::node_add_node(&C, tree, "GeometryNodeStoreNamedAttribute"_ustr);
    if (!tag) {
      return false;
    }
    session.helper_node_identifiers.append(tag->identifier);
    tag->parent = viewer.parent;
    STRNCPY_UTF8(tag->label, "Temporary Face Mapping");
    auto &storage = *static_cast<NodeGeometryStoreNamedAttribute *>(tag->storage);
    storage.data_type = CD_PROP_INT32;
    storage.domain = int8_t(AttrDomain::Face);
    BKE_ntree_update_tag_node_property(&tree, tag);
    BKE_main_ensure_invariants(*CTX_data_main(&C), tree.id);
    auto &name = bke::node_find_socket(*tag, SOCK_IN, "Name"_ustr)
                     ->default_value_typed<bNodeSocketValueString>()
                     ->value;
    STRNCPY_UTF8(name, session.top_attribute_name.c_str());
    session.recipe_links.remove_if([&](const EditRecipeLink &link) {
      return link.to_node == operation->identifier && link.to_socket == "Mesh";
    });
    if (!edit_link_add(tree,
                       session,
                       session.source_node_identifier,
                       session.source_socket_identifier,
                       tag->identifier,
                       "Geometry") ||
        !edit_link_add(tree, session, index->identifier, "Index", tag->identifier, "Value") ||
        !edit_link_add(tree, session, tag->identifier, "Geometry", operation->identifier, "Mesh"))
    {
      return false;
    }
  }
  if (extrude) {
    /* Offset has an implicit Normal field. An explicit constant overrides it, including at zero.
     * Top needs an actual consumer for its anonymous attribute to be materialized. */
    bNode *offset = bke::node_add_node(&C, tree, "FunctionNodeInputVector"_ustr);
    if (!offset) {
      return false;
    }
    session.helper_node_identifiers.append(offset->identifier);
    STRNCPY_UTF8(offset->label, "Extrusion Offset");
    offset->parent = viewer.parent;
    zero_v3(static_cast<NodeInputVector *>(offset->storage)->vector);
    if (!edit_link_add(
            tree, session, offset->identifier, "Vector", operation->identifier, "Offset"))
    {
      return false;
    }
  }
  if (extrude || loop_cut) {
    /* Anonymous Top/Loops needs an actual consumer before picking can read the field. */
    bNode *top = bke::node_add_node(&C, tree, "GeometryNodeStoreNamedAttribute"_ustr);
    if (!top) {
      return false;
    }
    session.helper_node_identifiers.append(top->identifier);
    session.preview_node_identifier = top->identifier;
    auto &storage = *static_cast<NodeGeometryStoreNamedAttribute *>(top->storage);
    storage.data_type = CD_PROP_BOOL;
    storage.domain = int8_t(session.edit_domain);
    STRNCPY_UTF8(top->label, "Temporary Viewer Picking");
    top->parent = viewer.parent;
    BKE_ntree_update_tag_node_property(&tree, top);
    BKE_main_ensure_invariants(*CTX_data_main(&C), tree.id);
    tree.ensure_topology_cache();
    top = tree.node_by_id(session.preview_node_identifier);
    auto &name = bke::node_find_socket(*top, SOCK_IN, "Name"_ustr)
                     ->default_value_typed<bNodeSocketValueString>()
                     ->value;
    STRNCPY_UTF8(name, session.top_attribute_name.c_str());
    /* Replace the provisional operation->Viewer link and its expected-link record. */
    session.recipe_links.remove_if([&](const EditRecipeLink &link) {
      return link.to_node == viewer.identifier &&
             link.to_socket == session.viewer_socket_identifier;
    });
    if (!edit_link_add(
            tree, session, operation->identifier, "Mesh", top->identifier, "Geometry") ||
        !edit_link_add(tree,
                       session,
                       operation->identifier,
                       loop_cut ? "Loops" : "Top",
                       top->identifier,
                       "Value") ||
        !edit_link_add(tree,
                       session,
                       top->identifier,
                       "Geometry",
                       viewer.identifier,
                       session.viewer_socket_identifier))
    {
      return false;
    }
  }
  if (scale_elements) {
    /* Center has an implicit Position field. A socket default alone cannot override it. */
    if (!edit_link_add(
            tree, session, capture->identifier, "Pivot", operation->identifier, "Center"))
    {
      return false;
    }
  }
  if (recipe == EditRecipe::Matrix) {
    const char *types[] = {"GeometryNodeInputPosition",
                           "FunctionNodeCombineMatrix",
                           "FunctionNodeTransformPoint",
                           "ShaderNodeVectorMath",
                           "ShaderNodeVectorMath"};
    for (const int i : IndexRange(5)) {
      bNode *helper = bke::node_add_node(&C, tree, UString(types[i]));
      if (!helper) {
        return false;
      }
      session.helper_node_identifiers.append(helper->identifier);
      helper->parent = viewer.parent;
      if (i >= 3) {
        helper->custom1 = i == 3 ? NODE_VECTOR_MATH_SUBTRACT : NODE_VECTOR_MATH_ADD;
        BKE_ntree_update_tag_node_property(&tree, helper);
      }
    }
    const int position = session.helper_node_identifiers[0];
    const int matrix = session.helper_node_identifiers[1];
    const int transform = session.helper_node_identifiers[2];
    const int subtract = session.helper_node_identifiers[3];
    const int add = session.helper_node_identifiers[4];
    if (!edit_link_add(tree, session, position, "Position", subtract, "Vector") ||
        !edit_link_add(tree, session, capture->identifier, "Pivot", subtract, "Vector_001") ||
        !edit_link_add(tree, session, subtract, "Vector", transform, "Vector") ||
        !edit_link_add(tree, session, matrix, "Matrix", transform, "Transform") ||
        !edit_link_add(tree, session, transform, "Vector", add, "Vector") ||
        !edit_link_add(tree, session, capture->identifier, "Pivot", add, "Vector_001") ||
        !edit_link_add(tree, session, add, "Vector", operation->identifier, "Position"))
    {
      return false;
    }
  }
  edit_recipe_layout(tree, viewer, session);
  return true;
}

struct ScaleRecipe {
  EditRecipe recipe;
  float factor;
  float3 axis;
};

static std::optional<ScaleRecipe> edit_scale_recipe(const float4x4 &matrix, const float3 &pivot)
{
  constexpr float tolerance = 1.0e-5f;
  /* Scale Elements' scalar/axis controls express s*I or I+(s-1)*axis*axis^T. In particular,
   * a world-axis scale under a nonuniform object transform may be shear in local coordinates. */
  float3x3 linear(matrix);
  const auto matrix_matches = [&](const float3x3 &candidate) {
    for (const int column : IndexRange(3)) {
      for (const int row : IndexRange(3)) {
        if (std::abs(linear[column][row] - candidate[column][row]) >
            tolerance * std::max(1.0f, std::abs(linear[column][row])))
        {
          return false;
        }
      }
    }
    return true;
  };
  if (!edit_position_matches(matrix.location(), pivot - linear * pivot)) {
    return std::nullopt;
  }
  const float uniform = linear[0][0];
  float3x3 candidate = float3x3::identity() * uniform;
  if (uniform >= 0.0f && matrix_matches(candidate)) {
    return ScaleRecipe{EditRecipe::UniformScale, uniform, float3(1, 0, 0)};
  }
  const float3x3 difference = linear - float3x3::identity();
  const float factor = 1.0f + difference[0][0] + difference[1][1] + difference[2][2];
  if (factor < 0.0f || std::abs(factor - 1.0f) < tolerance) {
    return std::nullopt;
  }
  int largest = 0;
  for (const int column : IndexRange(1, 2)) {
    if (math::length_squared(difference[column]) > math::length_squared(difference[largest])) {
      largest = column;
    }
  }
  const float3 axis = math::normalize(difference[largest]);
  candidate = float3x3::identity();
  for (const int column : IndexRange(3)) {
    for (const int row : IndexRange(3)) {
      candidate[column][row] += (factor - 1.0f) * axis[column] * axis[row];
    }
  }
  if (!matrix_matches(candidate)) {
    return std::nullopt;
  }
  return ScaleRecipe{EditRecipe::AxisScale, factor, axis};
}

static bool edit_recipe_values_update(bContext &C,
                                      bNodeTree &tree,
                                      bNode &viewer,
                                      GeometryViewerSelectionSession &session,
                                      const GeometryViewerTransform kind,
                                      const float3 &pivot)
{
  std::optional<ScaleRecipe> scale;
  EditRecipe recipe = kind == GeometryViewerTransform::Move ? EditRecipe::Offset :
                                                              EditRecipe::Matrix;
  if (ELEM(session.recipe, EditRecipe::Extrude, EditRecipe::Duplicate)) {
    if (kind != GeometryViewerTransform::Move) {
      return false;
    }
    recipe = session.recipe;
  }
  if (kind == GeometryViewerTransform::Scale && session.edit_domain != AttrDomain::Point) {
    scale = edit_scale_recipe(session.local_transform, pivot);
    if (scale) {
      recipe = scale->recipe;
    }
  }
  if (!edit_recipe_create(C, tree, viewer, session, recipe)) {
    return false;
  }
  tree.ensure_topology_cache();
  bNode &operation = *tree.node_by_id(session.operation_node_identifier);
  if (recipe == EditRecipe::Extrude) {
    session.offset = session.local_transform.location();
    bNode &offset = *tree.node_by_id(session.helper_node_identifiers[0]);
    copy_v3_v3(static_cast<NodeInputVector *>(offset.storage)->vector, session.offset);
    BKE_ntree_update_tag_node_property(&tree, &offset);
  }
  else if (ELEM(recipe, EditRecipe::Offset, EditRecipe::Duplicate)) {
    session.offset = session.local_transform.location();
    if (session.reusing_move) {
      session.offset += session.move_batch->offset;
    }
    copy_v3_v3(bke::node_find_socket(operation, SOCK_IN, "Offset"_ustr)
                   ->default_value_typed<bNodeSocketValueVector>()
                   ->value,
               session.offset);
  }
  else if (scale) {
    bke::node_find_socket(operation, SOCK_IN, "Scale"_ustr)
        ->default_value_typed<bNodeSocketValueFloat>()
        ->value = scale->factor;
    copy_v3_v3(bke::node_find_socket(operation, SOCK_IN, "Axis"_ustr)
                   ->default_value_typed<bNodeSocketValueVector>()
                   ->value,
               scale->axis);
    bke::node_find_socket(operation, SOCK_IN, "Scale Mode"_ustr)
        ->default_value_typed<bNodeSocketValueMenu>()
        ->value = recipe == EditRecipe::UniformScale ? GEO_NODE_SCALE_ELEMENTS_UNIFORM :
                                                       GEO_NODE_SCALE_ELEMENTS_SINGLE_AXIS;
  }
  else {
    bNode &matrix = *tree.node_by_id(session.helper_node_identifiers[1]);
    float4x4 centered = session.local_transform;
    centered.location() += float3x3(centered) * pivot - pivot;
    if (edit_position_matches(centered.location(), float3(0))) {
      centered.location() = float3(0);
    }
    for (const int column : IndexRange(4)) {
      for (const int row : IndexRange(4)) {
        const std::string socket = "Column " + std::to_string(column + 1) + " Row " +
                                   std::to_string(row + 1);
        bke::node_find_socket(matrix, SOCK_IN, UString(socket))
            ->default_value_typed<bNodeSocketValueFloat>()
            ->value = centered[column][row];
      }
    }
    BKE_ntree_update_tag_node_property(&tree, &matrix);
  }
  BKE_ntree_update_tag_node_property(&tree, &operation);
  session.generation++;
  edit_tree_tag(C, tree);
  return true;
}

/** Build the replacement alongside the pending recipe; do not remove its safety snapshot yet. */
static bool edit_rotation_group_create(bContext &C,
                                       bNodeTree &tree,
                                       bNode &viewer,
                                       GeometryViewerSelectionSession &session)
{
  Main &bmain = *CTX_data_main(&C);
  tree.ensure_topology_cache();
  const bNode *original_capture = tree.node_by_id(session.capture_node_identifier);
  if (!original_capture || !original_capture->storage || session.recipe != EditRecipe::Matrix ||
      session.transform_kind != GeometryViewerTransform::Rotate)
  {
    return false;
  }
  const auto &capture_data = *static_cast<const NodeGeometryCapturedSelection *>(
      original_capture->storage);
  if (nodes::geometry_edit::capture_signature(capture_data) != session.source_signature ||
      capture_data.domain != int8_t(session.edit_domain) ||
      Span<int>(capture_data.indices, capture_data.indices_num) !=
          session.capture_indices.as_span() ||
      Span<int>(capture_data.point_indices, capture_data.point_indices_num) !=
          session.capture_point_indices.as_span())
  {
    return false;
  }
  bNodeTree *group = bke::node_tree_add_tree(&bmain, "Set Rotation", "GeometryNodeTree");
  if (!group) {
    return false;
  }
  /* Track this initially unreferenced ID until a group node owns it, so rollback can discard it.
   */
  id_us_min(&group->id);
  session.rotation_group_tree_uid = group->id.session_uid;
  BKE_id_move_to_same_lib(bmain, group->id, tree.id);
  group->default_group_node_width = 220;
  bNodeTreeInterfaceSocket *mesh_in = group->tree_interface.add_socket(
      "Mesh",
      "The mesh stage on which the selection was captured",
      "NodeSocketGeometry",
      NODE_INTERFACE_SOCKET_INPUT,
      nullptr);
  bNodeTreeInterfaceSocket *rotation_in = group->tree_interface.add_socket(
      "Rotation",
      "Rotation in the captured viewport world axes, about the captured pivot",
      "NodeSocketRotation",
      NODE_INTERFACE_SOCKET_INPUT,
      nullptr);
  bNodeTreeInterfaceSocket *mesh_out = group->tree_interface.add_socket(
      "Mesh",
      "Mesh with the captured elements rotated",
      "NodeSocketGeometry",
      NODE_INTERFACE_SOCKET_OUTPUT,
      nullptr);
  if (!mesh_in || !rotation_in || !mesh_out) {
    return false;
  }
  const std::string mesh_input_identifier = mesh_in->identifier;
  const std::string mesh_output_identifier = mesh_out->identifier;
  session.rotation_socket_identifier = rotation_in->identifier;
  copy_v3_v3(static_cast<bNodeSocketValueRotation *>(rotation_in->socket_data)->value_euler,
             session.rotation_euler);

  const char *types[] = {"NodeGroupInput",
                         "NodeGroupOutput",
                         "GeometryNodeSelectByIndex",
                         "GeometryNodeInputPosition",
                         "ShaderNodeVectorMath",
                         "FunctionNodeCombineTransform",
                         "FunctionNodeCombineMatrix",
                         "FunctionNodeInvertMatrix",
                         "FunctionNodeMatrixMultiply",
                         "FunctionNodeMatrixMultiply",
                         "FunctionNodeTransformPoint",
                         "ShaderNodeVectorMath",
                         "GeometryNodeSetPosition"};
  Vector<bNode *> group_nodes;
  for (const char *type : types) {
    bNode *node = bke::node_add_node(&C, *group, UString(type));
    if (!node) {
      return false;
    }
    group_nodes.append(node);
  }
  bNode &input = *group_nodes[0], &output = *group_nodes[1], &capture = *group_nodes[2];
  bNode &position = *group_nodes[3], &subtract = *group_nodes[4], &rotation = *group_nodes[5];
  bNode &basis = *group_nodes[6], &inverse = *group_nodes[7], &rotate_basis = *group_nodes[8];
  bNode &local = *group_nodes[9], &transform = *group_nodes[10], &add = *group_nodes[11];
  bNode &operation = *group_nodes[12];
  output.flag |= NODE_DO_OUTPUT;
  capture.custom1 = original_capture->custom1;
  capture.custom2 = nodes::geometry_edit::SELECT_BY_INDEX_CAPTURED;
  nodes::geometry_edit::capture_copy(
      *static_cast<NodeGeometryCapturedSelection *>(capture.storage), capture_data);
  BKE_ntree_update_tag_node_property(group, &capture);
  subtract.custom1 = NODE_VECTOR_MATH_SUBTRACT;
  add.custom1 = NODE_VECTOR_MATH_ADD;
  BKE_ntree_update_tag_node_property(group, &subtract);
  BKE_ntree_update_tag_node_property(group, &add);
  STRNCPY_UTF8(rotation.label, "Editable Rotation");
  STRNCPY_UTF8(basis.label, "Captured Object Basis");
  STRNCPY_UTF8(inverse.label, "World to Object");
  STRNCPY_UTF8(rotate_basis.label, "World Rotation");
  STRNCPY_UTF8(local.label, "Object-space Rotation");
  STRNCPY_UTF8(subtract.label, "Subtract Pivot");
  STRNCPY_UTF8(add.label, "Restore Pivot");
  /* This constant affine basis is deliberate: A^-1 R A is not generally a pure local rotation
   * under unapplied nonuniform scale or parent shear. Do not decompose that local matrix to TRS.
   */
  const float4x4 object_basis = float4x4(float3x3(session.source_to_world));
  for (const int column : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      const std::string identifier = "Column " + std::to_string(column + 1) + " Row " +
                                     std::to_string(row + 1);
      bke::node_find_socket(basis, SOCK_IN, UString(identifier))
          ->default_value_typed<bNodeSocketValueFloat>()
          ->value = object_basis[column][row];
    }
  }
  BKE_ntree_update_tag_node_property(group, &basis);
  BKE_main_ensure_invariants(bmain, group->id);
  capture_indices_text_update(capture);
  const auto link = [&](bNode &from,
                        const std::string &from_identifier,
                        bNode &to,
                        const std::string &to_identifier) {
    bNodeSocket *from_socket = bke::node_find_socket(from, SOCK_OUT, UString(from_identifier));
    bNodeSocket *to_socket = bke::node_find_socket(to, SOCK_IN, UString(to_identifier));
    if (!from_socket || !to_socket) {
      return false;
    }
    bke::node_add_link(*group, from, *from_socket, to, *to_socket);
    return true;
  };
  if (!link(input, mesh_input_identifier, capture, "Geometry") ||
      !link(input, mesh_input_identifier, operation, "Geometry") ||
      !link(capture, "Points", operation, "Selection") ||
      !link(input, session.rotation_socket_identifier, rotation, "Rotation") ||
      !link(position, "Position", subtract, "Vector") ||
      !link(capture, "Pivot", subtract, "Vector_001") ||
      !link(basis, "Matrix", inverse, "Matrix") ||
      !link(rotation, "Transform", rotate_basis, "Matrix") ||
      !link(basis, "Matrix", rotate_basis, "Matrix_001") ||
      !link(inverse, "Matrix", local, "Matrix") ||
      !link(rotate_basis, "Matrix", local, "Matrix_001") ||
      !link(subtract, "Vector", transform, "Vector") ||
      !link(local, "Matrix", transform, "Transform") ||
      !link(transform, "Vector", add, "Vector") || !link(capture, "Pivot", add, "Vector_001") ||
      !link(add, "Vector", operation, "Position") ||
      !link(operation, "Geometry", output, mesh_output_identifier))
  {
    return false;
  }
  /* Internal nodes remain ordinary editable nodes, arranged along mesh and transform data flow. */
  const float2 locations[] = {{-900, 220},
                              {1260, 220},
                              {-660, 0},
                              {-660, 300},
                              {-410, 300},
                              {-660, -260},
                              {-900, -420},
                              {-650, -600},
                              {-410, -260},
                              {-150, -260},
                              {100, 300},
                              {400, 300},
                              {700, 220}};
  for (const int i : group_nodes.index_range()) {
    copy_v2_v2(group_nodes[i]->location, locations[i]);
  }
  BKE_main_ensure_invariants(bmain, group->id);
  bNode *instance = bke::node_add_node(&C, tree, "GeometryNodeGroup"_ustr);
  if (!instance) {
    return false;
  }
  session.rotation_group_node_identifier = instance->identifier;
  session.helper_node_identifiers.append(instance->identifier);
  instance->id = &group->id;
  id_us_plus(&group->id);
  instance->parent = viewer.parent;
  instance->width = 220;
  STRNCPY_UTF8(instance->name, "Set Rotation");
  bke::node_unique_name(tree, *instance);
  STRNCPY_UTF8(instance->label, "Set Rotation");
  copy_v2_v2(instance->location, original_capture->location);
  BKE_ntree_update_tag_node_property(&tree, instance);
  BKE_main_ensure_invariants(bmain, tree.id);
  bNodeSocket *rotation_socket = bke::node_find_socket(
      *instance, SOCK_IN, UString(session.rotation_socket_identifier));
  if (!rotation_socket) {
    return false;
  }
  copy_v3_v3(rotation_socket->default_value_typed<bNodeSocketValueRotation>()->value_euler,
             session.rotation_euler);
  session.recipe_links.remove_if([&](const EditRecipeLink &expected) {
    return expected.to_node == viewer.identifier &&
           expected.to_socket == session.viewer_socket_identifier;
  });
  if (!edit_link_add(tree,
                     session,
                     session.source_node_identifier,
                     session.source_socket_identifier,
                     instance->identifier,
                     mesh_input_identifier) ||
      !edit_link_add(tree,
                     session,
                     instance->identifier,
                     mesh_output_identifier,
                     viewer.identifier,
                     session.viewer_socket_identifier))
  {
    return false;
  }
  edit_tree_tag(C, tree);
  return true;
}

/** The evaluated group matches the original pending recipe; retire only the old root nodes. */
static void edit_rotation_group_finalize(bContext &C,
                                         bNodeTree &tree,
                                         bNode &viewer,
                                         GeometryViewerSelectionSession &session)
{
  const int instance_identifier = session.rotation_group_node_identifier;
  Vector<int> remove_nodes = session.helper_node_identifiers;
  remove_nodes.append(session.capture_node_identifier);
  remove_nodes.append(session.operation_node_identifier);
  for (const int identifier : remove_nodes) {
    if (identifier != instance_identifier) {
      tree.ensure_topology_cache();
      if (bNode *node = tree.node_by_id(identifier)) {
        bke::node_remove_node(CTX_data_main(&C), tree, *node, true);
      }
    }
  }
  tree.ensure_topology_cache();
  bNode &instance = *tree.node_by_id(instance_identifier);
  if (float2(viewer.location) == session.viewer_layout_location) {
    viewer.location[0] = instance.location[0] + instance.width + 80;
    viewer.location[1] = instance.location[1];
    session.viewer_layout_location = float2(viewer.location);
  }
  session.capture_node_identifier = 0;
  session.operation_node_identifier = instance_identifier;
  session.helper_node_identifiers.clear();
  session.recipe_links.clear();
  /* Clear ownership without deleting the now-durable group. Undo removes it with the graph edit.
   */
  session.rotation_group_tree_uid = 0;
  session.rotation_group_node_identifier = 0;
  session.rotation_socket_identifier.clear();
  session.rotation_group_preparing = false;
  edit_tree_tag(C, tree);
}

enum EditTransactionAction {
  EDIT_VALIDATE = 3,
  EDIT_ABORT_EXTRUSION = 4,
  EDIT_ABORT_LOOP_CUT = 5,
  EDIT_ABORT_DUPLICATION = 6
};

static wmOperatorStatus geometry_viewer_editing_toggle_exec(bContext *C, wmOperator *op)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  bNode *viewer = viewer_node_from_operator(*C, *op);
  if (!viewer || !viewer->storage) {
    return OPERATOR_CANCELLED;
  }
  auto &storage = *static_cast<NodeGeometryViewer *>(viewer->storage);
  if (storage.selection_enabled) {
    selection_session_finish(*C, *CTX_wm_space_node(C), *viewer);
    return OPERATOR_FINISHED;
  }
  return selection_enable(*C, *op, *viewer, true);
}

static wmOperatorStatus geometry_viewer_edit_transaction_exec(bContext *C, wmOperator *op)
{
  const int action = RNA_enum_get(op->ptr, "action");
  /* Evaluate before resolving runtime IDs: update callbacks can remove nodes or end a session. */
  if (!ELEM(action, EDIT_ABORT_EXTRUSION, EDIT_ABORT_LOOP_CUT, EDIT_ABORT_DUPLICATION)) {
    BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  }
  if (action == EDIT_VALIDATE) {
    geometry_viewer_selection_update(*C);
    return OPERATOR_FINISHED;
  }
  bNode *viewer = viewer_node_from_operator(*C, *op);
  if (!viewer || !viewer->storage) {
    return OPERATOR_CANCELLED;
  }
  auto &storage = *static_cast<NodeGeometryViewer *>(viewer->storage);
  auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
  if (!storage.editing_enabled || !session) {
    BKE_report(op->reports, RPT_ERROR, "Enable Viewer editing first");
    return OPERATOR_CANCELLED;
  }
  if (ELEM(action, EDIT_ABORT_EXTRUSION, EDIT_ABORT_LOOP_CUT, EDIT_ABORT_DUPLICATION)) {
    if (session->recipe != (action == EDIT_ABORT_EXTRUSION   ? EditRecipe::Extrude :
                            action == EDIT_ABORT_DUPLICATION ? EditRecipe::Duplicate :
                                                               EditRecipe::LoopCut))
    {
      return OPERATOR_CANCELLED;
    }
    session_end_by_uid(*C, storage.selection_object_session_uid);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

static wmOperatorStatus geometry_viewer_import_selection_exec(bContext *C, wmOperator *op)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  bNode *viewer_node = viewer_node_from_operator(*C, *op);
  if (!viewer_node || !viewer_node->storage) {
    return OPERATOR_CANCELLED;
  }

  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  NodeGeometryViewer &storage = *static_cast<NodeGeometryViewer *>(viewer_node->storage);
  if (!storage.selection_enabled) {
    BKE_report(op->reports, RPT_ERROR, "Viewer selection is not enabled");
    return OPERATOR_CANCELLED;
  }
  if (storage.editing_enabled && geometry_viewer_edit_has_pending(*viewer_node)) {
    BKE_report(
        op->reports, RPT_INFO, "Confirm or cancel the pending edit before importing a selection");
    return OPERATOR_CANCELLED;
  }

  Object *temporary_object = find_object(bmain, storage.selection_object_session_uid);
  BMEditMesh *edit_mesh = temporary_object ? BKE_editmesh_from_object(temporary_object) : nullptr;
  GeometryViewerSelectionSession *session = selection_sessions().lookup_ptr(
      storage.selection_object_session_uid);
  if (!temporary_object || !edit_mesh || !session) {
    BKE_report(op->reports, RPT_ERROR, "The Viewer selection session is no longer available");
    return OPERATOR_CANCELLED;
  }
  if (!selection_topology_is_unchanged(*edit_mesh->bm, *session)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "The temporary Viewer mesh topology changed; disable selection and try again");
    return OPERATOR_CANCELLED;
  }
  const Mesh *current_mesh = viewer_mesh_get(snode, *viewer_node);
  if (!current_mesh ||
      nodes::geometry_edit::mesh_signature(*current_mesh) != session->source_signature)
  {
    BKE_report(
        op->reports, RPT_ERROR, "The Viewer input stage changed; exit Edit Mode and recapture");
    return OPERATOR_CANCELLED;
  }

  const std::optional<AttrDomain> active_domain = selection_domain_from_mode(
      edit_mesh->selectmode);
  if (!active_domain) {
    BKE_report(op->reports, RPT_ERROR, "Unsupported viewport mesh selection mode");
    return OPERATOR_CANCELLED;
  }
  const AttrDomain domain = *active_domain;
  storage.selection_domain = int8_t(domain);
  const Vector<int> indices = selected_indices_get(*edit_mesh->bm, domain);
  if (indices.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No Viewer mesh elements are selected");
    return OPERATOR_CANCELLED;
  }
  {
    bNodeTree &tree = *snode.edittree;
    bNodeLink *source = viewer_geometry_link(tree, *viewer_node);
    if (!source) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "A captured selection requires a single linked Viewer mesh input");
      return OPERATOR_CANCELLED;
    }
    bNode *capture = bke::node_add_node(C, tree, "GeometryNodeSelectByIndex"_ustr);
    if (!capture) {
      return OPERATOR_CANCELLED;
    }
    capture->custom1 = int16_t(domain);
    capture->custom2 = nodes::geometry_edit::SELECT_BY_INDEX_CAPTURED;
    nodes::geometry_edit::capture_initialize(
        *static_cast<NodeGeometryCapturedSelection *>(capture->storage),
        session->source_signature,
        domain,
        indices,
        affected_points_get(*edit_mesh->bm, domain));
    BKE_ntree_update_tag_node_property(&tree, capture);
    BKE_main_ensure_invariants(bmain, tree.id);
    capture->parent = viewer_node->parent;
    capture->location[0] = viewer_node->location[0] + viewer_node->width + 40;
    capture->location[1] = viewer_node->location[1];
    capture_indices_text_update(*capture);
    bNodeSocket *geometry = bke::node_find_socket(*capture, SOCK_IN, "Geometry"_ustr);
    if (!geometry) {
      bke::node_remove_node(&bmain, tree, *capture, true);
      return OPERATOR_CANCELLED;
    }
    bke::node_add_link(tree, *source->fromnode, *source->fromsock, *capture, *geometry);
    node_deselect_all(tree);
    bke::node_set_selected(*capture, true);
    ED_node_set_active(&bmain, &snode, &tree, capture, nullptr);
    selection_session_finish(*C, snode, *viewer_node);
    edit_tree_tag(*C, tree);
    return OPERATOR_FINISHED;
  }
}

/** Retire runtime IDs before the operator's single global undo snapshot. The next refresh creates
 * a fresh picking proxy from the committed graph, never from a possibly reordered preview. */
static void edit_transaction_commit(bContext &C,
                                    SpaceNode &owner,
                                    bNodeTree &tree,
                                    bNode &viewer,
                                    GeometryViewerSelectionSession &session,
                                    std::optional<GeometryViewerEditResume> resume)
{
  if (resume) {
    resume->session.native_operator = nullptr;
  }
  if (session.preview_node_identifier) {
    const bool duplicating = session.recipe == EditRecipe::Duplicate;
    tree.ensure_topology_cache();
    if (bNode *preview = tree.node_by_id(session.preview_node_identifier)) {
      bke::node_remove_node(CTX_data_main(&C), tree, *preview, true);
    }
    tree.ensure_topology_cache();
    bNode &operation = *tree.node_by_id(session.operation_node_identifier);
    if (duplicating) {
      bNode &join = *tree.node_by_id(session.duplicate_join_node_identifier);
      bNodeSocket &input = *bke::node_find_socket(join, SOCK_IN, "Geometry"_ustr);
      Vector<bNodeLink *> remove_links;
      for (bNodeLink &link : tree.links) {
        if (link.tosock == &input) {
          remove_links.append(&link);
        }
      }
      for (bNodeLink *link : remove_links) {
        bke::node_remove_link(&tree, *link);
      }
      /* Preserve the preview's exact multi-input order: source first, duplicate second. A new
       * link beside the existing one would reuse its sort ID after removing the picking node. */
      bke::node_add_link(tree,
                         operation,
                         *bke::node_find_socket(operation, SOCK_OUT, "Geometry"_ustr),
                         join,
                         input);
      bNode &source = *tree.node_by_id(session.source_node_identifier);
      bke::node_add_link(
          tree,
          source,
          *bke::node_find_socket(source, SOCK_OUT, UString(session.source_socket_identifier)),
          join,
          input);
    }
    else {
      bke::node_add_link(
          tree,
          operation,
          *bke::node_find_socket(operation, SOCK_OUT, "Mesh"_ustr),
          viewer,
          *bke::node_find_socket(viewer, SOCK_IN, UString(session.viewer_socket_identifier)));
    }
    session.preview_node_identifier = 0;
    if (resume) {
      resume->session.preview_node_identifier = 0;
    }
    edit_recipe_layout(tree, viewer, session);
    edit_tree_tag(C, tree);
  }
  session.viewer_original_location.reset();
  if (resume) {
    resume->session.viewer_original_location.reset();
  }
  session.operation_node_identifier = session.capture_node_identifier = 0;
  session.helper_node_identifiers.clear();
  session.recipe_links.clear();
  selection_session_finish(C, owner, viewer);
  edit_resume() = std::move(resume);
}

static bool geometry_viewer_edit_tool_poll(bContext *C)
{
  const Object *object = CTX_data_edit_object(C);
  const auto *session = object ? selection_sessions().lookup_ptr(object->id.session_uid) : nullptr;
  const ARegion *region = CTX_wm_region(C);
  return CTX_wm_view3d(C) && region && region->regiontype == RGN_TYPE_WINDOW &&
         CTX_wm_region_view3d(C) && session && session->editing && !session->native_operator;
}

enum ViewerDeleteType {
  VIEWER_DELETE_VERTICES,
  VIEWER_DELETE_EDGES,
  VIEWER_DELETE_FACES,
  VIEWER_DELETE_EDGE_FACE,
  VIEWER_DELETE_ONLY_FACE,
  VIEWER_DISSOLVE_VERTICES,
  VIEWER_DISSOLVE_EDGES,
  VIEWER_DISSOLVE_FACES,
};

static wmOperatorStatus geometry_viewer_edit_delete_exec(bContext *C, wmOperator *op)
{
  /* The menu itself owns no nodes or snapshot. Resolve the current session only when the user
   * chooses an action, and again after evaluation, which can run arbitrary update handlers. */
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Main *main = CTX_data_main(C);
  Object *proxy = CTX_data_edit_object(C);
  const uint32_t uid = proxy ? proxy->id.session_uid : 0;
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Confirm or cancel the pending Viewer edit before deleting or dissolving");
    return OPERATOR_CANCELLED;
  }
  SpaceNode *owner = find_owner_space(*main, *session, true);
  bNodeTree *tree = find_tree(*main, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer ||
      !edit_session_validate(*C, *owner, *viewer, *session, true))
  {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return OPERATOR_CANCELLED;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  const auto picking_domain = selection_domain_from_mode(em->selectmode);
  if (!picking_domain ||
      !ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE))
  {
    BKE_report(op->reports, RPT_WARNING, "Choose a single mesh selection domain first");
    return OPERATOR_CANCELLED;
  }
  /* These node kernels can reorder every domain. Never reapply locally hidden elements using
   * stale indices on the result. A future hidden-element mapping must be explicit. */
  for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    if (!selected_indices_get(*em->bm, domain, BM_ELEM_HIDDEN).is_empty()) {
      BKE_report(
          op->reports, RPT_WARNING, "Reveal hidden Viewer elements before deleting or dissolving");
      return OPERATOR_CANCELLED;
    }
  }

  const auto type = ViewerDeleteType(RNA_enum_get(op->ptr, "type"));
  const bool dissolving = ELEM(
      type, VIEWER_DISSOLVE_VERTICES, VIEWER_DISSOLVE_EDGES, VIEWER_DISSOLVE_FACES);
  const AttrDomain domain =
      ELEM(type, VIEWER_DELETE_VERTICES, VIEWER_DISSOLVE_VERTICES) ? AttrDomain::Point :
      ELEM(type, VIEWER_DELETE_EDGES, VIEWER_DELETE_EDGE_FACE, VIEWER_DISSOLVE_EDGES) ?
                                                                     AttrDomain::Edge :
                                                                     AttrDomain::Face;
  if (selected_indices_get(*em->bm, domain).is_empty()) {
    BKE_report(op->reports, RPT_INFO, "Nothing is selected in the chosen operation's domain");
    return OPERATOR_CANCELLED;
  }
  session->delete_mode = type == VIEWER_DELETE_EDGE_FACE ?
                             GEO_NODE_DELETE_GEOMETRY_MODE_EDGE_FACE :
                         type == VIEWER_DELETE_ONLY_FACE ?
                             GEO_NODE_DELETE_GEOMETRY_MODE_ONLY_FACE :
                             GEO_NODE_DELETE_GEOMETRY_MODE_ALL;
  const uint32_t tree_uid = tree->id.session_uid;
  const int viewer_identifier = viewer->identifier;
  session->deletion_preparing = true;
  if (!edit_transaction_begin(*C, *tree, *viewer, *proxy, *session, std::nullopt, domain) ||
      !edit_recipe_create(
          *C, *tree, *viewer, *session, dissolving ? EditRecipe::Dissolve : EditRecipe::Delete))
  {
    edit_transaction_rollback(*C, *session);
    session->deletion_preparing = false;
    session->edit_domain = *picking_domain;
    return OPERATOR_CANCELLED;
  }
  edit_tree_tag(*C, *tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), main);
  if (CTX_data_main(C) != main || selection_sessions().lookup_ptr(uid) != session ||
      find_object(*main, uid) != proxy || find_tree(*main, tree_uid) != tree ||
      find_owner_space(*main, *session, true) != owner)
  {
    session_end_by_uid(*C, uid);
    return OPERATOR_CANCELLED;
  }
  tree->ensure_topology_cache();
  if (tree->node_by_id(viewer_identifier) != viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(*C, *owner, *viewer, *session, false))
  {
    session_end_by_uid(*C, uid);
    BKE_report(
        op->reports, RPT_WARNING, "Viewer edit cancelled because its input or context changed");
    return OPERATOR_CANCELLED;
  }
  const bke::GeometrySet *geometry = viewer_geometry_get(*owner, *viewer);
  bool valid = geometry && !geometry->has_instances() && !geometry->has_curves() &&
               !geometry->has_pointcloud() && !geometry->has_volume() &&
               !geometry->has_grease_pencil();
  auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(*owner);
  if (auto *log = logs.get_main_tree_log(*viewer)) {
    log->ensure_node_warnings(*main);
    for (const int identifier :
         {session->capture_node_identifier, session->operation_node_identifier})
    {
      if (const auto *node_log = log->find_node_log(identifier)) {
        valid &= node_log->warnings.is_empty();
      }
    }
  }
  const Mesh *result = geometry ? geometry->get_mesh() : nullptr;
  if (!valid || (dissolving && !result)) {
    session_end_by_uid(*C, uid);
    BKE_report(
        op->reports, RPT_WARNING, "The captured Viewer operation could not be evaluated safely");
    return OPERATOR_CANCELLED;
  }
  if (result && nodes::geometry_edit::mesh_signature(*result) == session->source_signature) {
    edit_transaction_rollback(*C, *session);
    session->deletion_preparing = false;
    session->edit_domain = *picking_domain;
    BKE_report(op->reports, RPT_INFO, "The selected elements produce no topology change");
    return OPERATOR_CANCELLED;
  }

  std::optional<GeometryViewerEditResume> resume;
  if (result && result->verts_num > 0) {
    resume.emplace(
        *session, session->operation_node_identifier, Vector<float3>(result->vert_positions()));
    auto &saved = resume->session;
    saved.proxy_signature = nodes::geometry_edit::mesh_signature(*result);
    saved.positions = resume->positions;
    saved.verts_num = result->verts_num;
    saved.edges_num = result->edges_num;
    saved.faces_num = result->faces_num;
    saved.edit_domain = *picking_domain;
    /* Delete and Dissolve have no authoritative result-selection output. Clear selection and
     * active history instead of guessing indices after a topology-changing operation. */
    saved.picked_indices.clear();
    saved.point_indices.clear();
    saved.hidden_verts.clear();
    saved.hidden_edges.clear();
    saved.hidden_faces.clear();
    saved.active_element_type = 0;
    saved.active_element_index = -1;
    saved.deletion_preparing = false;
  }
  session->deletion_preparing = false;
  /* A fully deleted mesh is a valid committed graph, but cannot supply a new picking proxy. */
  edit_transaction_commit(*C, *owner, *tree, *viewer, *session, std::move(resume));
  return OPERATOR_FINISHED;
}

enum ViewerFaceTool {
  VIEWER_FACE_TRIANGULATE,
  VIEWER_FACE_FLIP,
  VIEWER_FACE_SMOOTH,
  VIEWER_FACE_FLAT,
};

static wmOperatorStatus geometry_viewer_edit_face_exec(bContext *C, wmOperator *op)
{
  const auto type = ViewerFaceTool(RNA_enum_get(op->ptr, "type"));
  const EditRecipe recipe = type == VIEWER_FACE_TRIANGULATE ? EditRecipe::Triangulate :
                            type == VIEWER_FACE_FLIP        ? EditRecipe::FlipFaces :
                                                              EditRecipe::ShadeSmooth;
  const int quad_method = RNA_enum_get(op->ptr, "quad_method");
  const int ngon_method = RNA_enum_get(op->ptr, "ngon_method");
  if (!ELEM(
          type, VIEWER_FACE_TRIANGULATE, VIEWER_FACE_FLIP, VIEWER_FACE_SMOOTH, VIEWER_FACE_FLAT) ||
      quad_method < int(geometry::TriangulateQuadMode::Beauty) ||
      quad_method > int(geometry::TriangulateQuadMode::LongEdge) ||
      !ELEM(ngon_method,
            int(geometry::TriangulateNGonMode::Beauty),
            int(geometry::TriangulateNGonMode::EarClip)))
  {
    return OPERATOR_CANCELLED;
  }
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Main *main = CTX_data_main(C);
  Object *proxy = CTX_data_edit_object(C);
  const uint32_t uid = proxy ? proxy->id.session_uid : 0;
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(op->reports, RPT_WARNING, "Confirm or cancel the pending Viewer edit first");
    return OPERATOR_CANCELLED;
  }
  SpaceNode *owner = find_owner_space(*main, *session, true);
  bNodeTree *tree = find_tree(*main, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer ||
      !edit_session_validate(*C, *owner, *viewer, *session, true))
  {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return OPERATOR_CANCELLED;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  if (em->selectmode != SCE_SELECT_FACE) {
    BKE_report(op->reports, RPT_WARNING, "Choose Face selection mode for Viewer face tools");
    return OPERATOR_CANCELLED;
  }
  const Vector<int> selected = selected_indices_get(*em->bm, AttrDomain::Face);
  if (selected.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No Viewer faces are selected");
    return OPERATOR_CANCELLED;
  }
  session->hidden_verts = selected_indices_get(*em->bm, AttrDomain::Point, BM_ELEM_HIDDEN);
  session->hidden_edges = selected_indices_get(*em->bm, AttrDomain::Edge, BM_ELEM_HIDDEN);
  session->hidden_faces = selected_indices_get(*em->bm, AttrDomain::Face, BM_ELEM_HIDDEN);
  if (recipe == EditRecipe::Triangulate &&
      (!session->hidden_verts.is_empty() || !session->hidden_edges.is_empty() ||
       !session->hidden_faces.is_empty()))
  {
    BKE_report(op->reports, RPT_WARNING, "Reveal hidden Viewer elements before triangulating");
    return OPERATOR_CANCELLED;
  }
  const Mesh &input = *viewer_mesh_get(*owner, *viewer);
  const bool smooth = type == VIEWER_FACE_SMOOTH;
  if (recipe == EditRecipe::ShadeSmooth) {
    const VArray<bool> sharp =
        input.attributes().lookup_or_default<bool>("sharp_face", AttrDomain::Face, false).varray;
    if (std::all_of(selected.begin(),
                    selected.end(),
                    [&](const int index) { return sharp[index] == !smooth; }))
    {
      BKE_report(op->reports, RPT_INFO, "The selected faces already have this shading");
      return OPERATOR_CANCELLED;
    }
  }
  else if (recipe == EditRecipe::Triangulate &&
           std::all_of(selected.begin(),
                       selected.end(),
                       [&](const int index) { return input.faces()[index].size() == 3; }))
  {
    BKE_report(op->reports, RPT_INFO, "The selected faces are already triangles");
    return OPERATOR_CANCELLED;
  }
  /* Evaluation and update handlers may retire the old evaluated mesh. Keep an owned input for
   * correspondence checks instead of retaining pointers into the Viewer log across evaluation. */
  const bke::GeometrySet source_geometry = bke::GeometrySet::from_mesh(
      BKE_mesh_copy_for_eval(input));
  const Mesh &source_mesh = *source_geometry.get_mesh();
  const uint32_t tree_uid = session->tree_session_uid;
  const int viewer_identifier = session->viewer_identifier;
  session->face_smooth = smooth;
  session->triangulate_quad_method = quad_method;
  session->triangulate_ngon_method = ngon_method;
  session->face_tool_preparing = true;
  if (!edit_transaction_begin(*C, *tree, *viewer, *proxy, *session)) {
    edit_transaction_rollback(*C, *session);
    session->face_tool_preparing = false;
    return OPERATOR_CANCELLED;
  }
  if (recipe == EditRecipe::Triangulate) {
    const std::string base = ".viewer_face_source_" +
                             std::to_string(session->capture_node_identifier);
    session->top_attribute_name = base;
    for (int suffix = 1; source_mesh.attributes().contains(session->top_attribute_name); suffix++)
    {
      session->top_attribute_name = base + "_" + std::to_string(suffix);
    }
  }
  if (!edit_recipe_create(*C, *tree, *viewer, *session, recipe)) {
    edit_transaction_rollback(*C, *session);
    return OPERATOR_CANCELLED;
  }
  edit_tree_tag(*C, *tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), main);
  if (CTX_data_main(C) != main || selection_sessions().lookup_ptr(uid) != session ||
      find_object(*main, uid) != proxy || find_tree(*main, tree_uid) != tree ||
      find_owner_space(*main, *session, true) != owner)
  {
    session_end_by_uid(*C, uid);
    return OPERATOR_CANCELLED;
  }
  tree->ensure_topology_cache();
  if (tree->node_by_id(viewer_identifier) != viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(*C, *owner, *viewer, *session, false))
  {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "Viewer face edit cancelled because its context changed");
    return OPERATOR_CANCELLED;
  }
  const bke::GeometrySet *geometry = viewer_geometry_get(*owner, *viewer);
  const Mesh *result = geometry ? geometry->get_mesh() : nullptr;
  bool valid = result && !geometry->has_instances() && !geometry->has_curves() &&
               !geometry->has_pointcloud() && !geometry->has_volume() &&
               !geometry->has_grease_pencil();
  auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(*owner);
  if (auto *log = logs.get_main_tree_log(*viewer)) {
    log->ensure_node_warnings(*main);
    for (const int identifier :
         {session->capture_node_identifier, session->operation_node_identifier})
    {
      if (const auto *node_log = log->find_node_log(identifier)) {
        valid &= node_log->warnings.is_empty();
      }
    }
  }
  if (!valid || !edit_positions_match(source_mesh.vert_positions(), result->vert_positions())) {
    session_end_by_uid(*C, uid);
    BKE_report(
        op->reports, RPT_WARNING, "The captured face operation could not be evaluated safely");
    return OPERATOR_CANCELLED;
  }
  GeometryViewerEditResume resume{
      *session, session->operation_node_identifier, Vector<float3>(result->vert_positions())};
  auto &saved = resume.session;
  saved.proxy_signature = nodes::geometry_edit::mesh_signature(*result);
  saved.positions = resume.positions;
  saved.verts_num = result->verts_num;
  saved.edges_num = result->edges_num;
  saved.faces_num = result->faces_num;
  /* Face mode can retain history from another selection domain. Only restore a selected face. */
  const int active_source = saved.active_element_type == BM_FACE &&
                                    selected.contains(saved.active_element_index) ?
                                saved.active_element_index :
                                selected.first();
  saved.active_element_type = BM_FACE;
  saved.active_element_index = active_source;
  if (recipe == EditRecipe::Triangulate) {
    const bke::AttributeReader<int> lineage = result->attributes().lookup<int>(
        session->top_attribute_name, AttrDomain::Face);
    Vector<int> counts(source_mesh.faces_num, 0);
    Vector<int> expected(source_mesh.faces_num, 1);
    for (const int face : selected) {
      expected[face] = source_mesh.faces()[face].size() - 2;
    }
    valid = bool(lineage);
    saved.picked_indices.clear();
    saved.active_element_index = -1;
    if (lineage) {
      for (const int face : IndexRange(result->faces_num)) {
        const int original = lineage.varray[face];
        if (original < 0 || original >= source_mesh.faces_num) {
          valid = false;
          break;
        }
        counts[original]++;
        if (std::binary_search(selected.begin(), selected.end(), original)) {
          saved.picked_indices.append(face);
          if (original == active_source && saved.active_element_index < 0) {
            saved.active_element_index = face;
          }
        }
      }
    }
    if (!valid || counts != expected || saved.picked_indices.is_empty()) {
      session_end_by_uid(*C, uid);
      BKE_report(
          op->reports, RPT_WARNING, "The triangulated face selection could not be mapped safely");
      return OPERATOR_CANCELLED;
    }
    /* Removing the temporary upstream tag restores the exact ordinary Triangulate recipe.
     * Vertex order is invariant, so the affected point selection remains the original union. */
    for (const int identifier : session->helper_node_identifiers) {
      tree->ensure_topology_cache();
      bke::node_remove_node(main, *tree, *tree->node_by_id(identifier), true);
    }
    session->helper_node_identifiers.clear();
    session->recipe_links.clear();
    saved.helper_node_identifiers.clear();
    saved.recipe_links.clear();
    if (!edit_link_add(*tree,
                       *session,
                       session->source_node_identifier,
                       session->source_socket_identifier,
                       session->operation_node_identifier,
                       "Mesh"))
    {
      session_end_by_uid(*C, uid);
      return OPERATOR_CANCELLED;
    }
    edit_recipe_layout(*tree, *viewer, *session);
    edit_tree_tag(*C, *tree);
  }
  else if (result->faces_num != source_mesh.faces_num || result->edges() != source_mesh.edges() ||
           result->face_offsets() != source_mesh.face_offsets())
  {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "The face operation changed element correspondence");
    return OPERATOR_CANCELLED;
  }
  saved.face_tool_preparing = session->face_tool_preparing = false;
  edit_transaction_commit(*C, *owner, *tree, *viewer, *session, std::move(resume));
  return OPERATOR_FINISHED;
}

static wmOperatorStatus geometry_viewer_edit_merge_exec(bContext *C, wmOperator *op)
{
  const float distance = RNA_float_get(op->ptr, "distance");
  const int mode = RNA_enum_get(op->ptr, "mode");
  if (!std::isfinite(distance) || distance < 0 ||
      !ELEM(mode,
            GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL,
            GEO_NODE_MERGE_BY_DISTANCE_MODE_CONNECTED))
  {
    BKE_report(
        op->reports, RPT_ERROR, "Choose a valid merge mode and a finite nonnegative distance");
    return OPERATOR_CANCELLED;
  }
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Main *main = CTX_data_main(C);
  Object *proxy = CTX_data_edit_object(C);
  const uint32_t uid = proxy ? proxy->id.session_uid : 0;
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(
        op->reports, RPT_WARNING, "Confirm or cancel the pending Viewer edit before merging");
    return OPERATOR_CANCELLED;
  }
  SpaceNode *owner = find_owner_space(*main, *session, true);
  bNodeTree *tree = find_tree(*main, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer ||
      !edit_session_validate(*C, *owner, *viewer, *session, true))
  {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return OPERATOR_CANCELLED;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  const auto domain = selection_domain_from_mode(em->selectmode);
  if (!domain || !ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    BKE_report(op->reports, RPT_WARNING, "Choose one selection domain before merging");
    return OPERATOR_CANCELLED;
  }
  for (const AttrDomain hidden_domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    if (!selected_indices_get(*em->bm, hidden_domain, BM_ELEM_HIDDEN).is_empty()) {
      BKE_report(op->reports, RPT_WARNING, "Reveal hidden Viewer elements before merging");
      return OPERATOR_CANCELLED;
    }
  }
  if (affected_points_get(*em->bm, *domain).size() < 2) {
    BKE_report(op->reports, RPT_INFO, "Select at least two affected vertices before merging");
    return OPERATOR_CANCELLED;
  }
  session->merge_mode = GeometryNodeMergeByDistanceMode(mode);
  session->merge_distance = distance;
  session->merge_preparing = true;
  const uint32_t tree_uid = session->tree_session_uid;
  const int viewer_identifier = session->viewer_identifier;
  if (!edit_transaction_begin(*C, *tree, *viewer, *proxy, *session) ||
      !edit_recipe_create(*C, *tree, *viewer, *session, EditRecipe::Merge))
  {
    edit_transaction_rollback(*C, *session);
    session->merge_preparing = false;
    return OPERATOR_CANCELLED;
  }
  edit_tree_tag(*C, *tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), main);
  if (CTX_data_main(C) != main || selection_sessions().lookup_ptr(uid) != session ||
      find_object(*main, uid) != proxy || find_tree(*main, tree_uid) != tree ||
      find_owner_space(*main, *session, true) != owner)
  {
    session_end_by_uid(*C, uid);
    return OPERATOR_CANCELLED;
  }
  tree->ensure_topology_cache();
  if (tree->node_by_id(viewer_identifier) != viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(*C, *owner, *viewer, *session, false))
  {
    session_end_by_uid(*C, uid);
    BKE_report(
        op->reports, RPT_WARNING, "Viewer merge cancelled because its input or context changed");
    return OPERATOR_CANCELLED;
  }
  const bke::GeometrySet *geometry = viewer_geometry_get(*owner, *viewer);
  const Mesh *result = geometry ? geometry->get_mesh() : nullptr;
  bool valid = result && !geometry->has_instances() && !geometry->has_curves() &&
               !geometry->has_pointcloud() && !geometry->has_volume() &&
               !geometry->has_grease_pencil();
  auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(*owner);
  if (auto *log = logs.get_main_tree_log(*viewer)) {
    log->ensure_node_warnings(*main);
    for (const int identifier :
         {session->capture_node_identifier, session->operation_node_identifier})
    {
      if (const auto *node_log = log->find_node_log(identifier)) {
        valid &= node_log->warnings.is_empty();
      }
    }
  }
  if (!valid) {
    session_end_by_uid(*C, uid);
    BKE_report(op->reports, RPT_WARNING, "The captured merge could not be evaluated safely");
    return OPERATOR_CANCELLED;
  }
  if (result->verts_num == session->source_signature.verts_num) {
    edit_transaction_rollback(*C, *session);
    session->merge_preparing = false;
    BKE_report(op->reports,
               RPT_INFO,
               "No selected vertices are within the chosen merge distance and mode");
    return OPERATOR_CANCELLED;
  }
  GeometryViewerEditResume resume{
      *session, session->operation_node_identifier, Vector<float3>(result->vert_positions())};
  auto &saved = resume.session;
  saved.proxy_signature = nodes::geometry_edit::mesh_signature(*result);
  saved.positions = resume.positions;
  saved.verts_num = result->verts_num;
  saved.edges_num = result->edges_num;
  saved.faces_num = result->faces_num;
  /* Merge has no result-selection field. Do not reinterpret pre-merge indices on new topology. */
  saved.picked_indices.clear();
  saved.point_indices.clear();
  saved.active_element_type = 0;
  saved.active_element_index = -1;
  saved.hidden_verts.clear();
  saved.hidden_edges.clear();
  saved.hidden_faces.clear();
  saved.merge_preparing = false;
  session->merge_preparing = false;
  edit_transaction_commit(*C, *owner, *tree, *viewer, *session, std::move(resume));
  return OPERATOR_FINISHED;
}

/** Only called before TransInfo exists. Never replace topology during a native gesture. */
static bool edit_topology_proxy_replace(bContext &C,
                                        Object &proxy,
                                        GeometryViewerSelectionSession &session,
                                        const Mesh &result_mesh,
                                        const Span<int> picked)
{
  if (picked.is_empty()) {
    return false;
  }
  Main &bmain = *CTX_data_main(&C);
  const auto result_signature = nodes::geometry_edit::mesh_signature(result_mesh);
  Mesh *result_copy = BKE_mesh_copy_for_eval(result_mesh);
  session.positions = Vector<float3>(result_mesh.vert_positions());
  if (!ed::object::editmode_exit_ex(&bmain, CTX_data_scene(&C), &proxy, ed::object::EM_FREEDATA)) {
    BKE_id_free(nullptr, result_copy);
    return false;
  }
  Mesh *proxy_mesh = id_cast<Mesh *>(proxy.data);
  BKE_mesh_nomain_to_mesh(result_copy, proxy_mesh, &proxy, false);
  proxy_mesh->id.tag |= ID_TAG_RUNTIME;
  if (!ed::object::editmode_enter_ex(&bmain, CTX_data_scene(&C), &proxy, 0)) {
    return false;
  }
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  EDBM_flag_disable_all(em->bm, BM_ELEM_SELECT);
  EDBM_flag_disable_all(em->bm, BM_ELEM_HIDDEN);
  EDBM_selectmode_set(em, em->bm, selection_mode_from_domain(session.edit_domain));
  BM_mesh_elem_table_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);
  for (const int index : picked) {
    switch (session.edit_domain) {
      case AttrDomain::Point:
        BM_vert_select_set(em->bm, BM_vert_at_index(em->bm, index), true);
        break;
      case AttrDomain::Edge:
        BM_edge_select_set(em->bm, BM_edge_at_index(em->bm, index), true);
        break;
      case AttrDomain::Face:
        BM_face_select_set(em->bm, BM_face_at_index(em->bm, index), true);
        break;
      default:
        BLI_assert_unreachable();
    }
  }
  EDBM_selectmode_flush(em->bm, em->selectmode);
  for (const int index : session.hidden_verts) {
    BM_elem_flag_enable(BM_vert_at_index(em->bm, index), BM_ELEM_HIDDEN);
  }
  for (const int index : session.hidden_edges) {
    BM_elem_flag_enable(BM_edge_at_index(em->bm, index), BM_ELEM_HIDDEN);
  }
  for (const int index : session.hidden_faces) {
    BM_elem_flag_enable(BM_face_at_index(em->bm, index), BM_ELEM_HIDDEN);
  }
  session.active_element_index = picked[0];
  switch (session.edit_domain) {
    case AttrDomain::Point:
      session.active_element_type = BM_VERT;
      BM_select_history_store(em->bm, BM_vert_at_index(em->bm, picked[0]));
      break;
    case AttrDomain::Edge:
      session.active_element_type = BM_EDGE;
      BM_select_history_store(em->bm, BM_edge_at_index(em->bm, picked[0]));
      break;
    case AttrDomain::Face:
      session.active_element_type = BM_FACE;
      BM_mesh_active_face_set(em->bm, BM_face_at_index(em->bm, picked[0]));
      BM_select_history_store(em->bm, BM_face_at_index(em->bm, picked[0]));
      break;
    default:
      BLI_assert_unreachable();
  }
  session.picked_indices = Vector<int>(picked);
  session.point_indices = affected_points_get(*em->bm, session.edit_domain);
  session.verts_num = em->bm->totvert;
  session.edges_num = em->bm->totedge;
  session.faces_num = em->bm->totface;
  session.proxy_signature = proxy_signature_get(*em->bm);
  if (session.proxy_signature != result_signature) {
    return false;
  }
  EDBM_update_extern(proxy_mesh, true, true);
  return true;
}

static bool edit_duplication_prepare(bContext &C,
                                     wmOperator &op,
                                     Object &proxy,
                                     GeometryViewerSelectionSession &session)
{
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, session, true);
  bNodeTree *tree = find_tree(bmain, session.tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session.viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, session, true))
  {
    BKE_report(op.reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return false;
  }
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  if (!ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    BKE_report(op.reports, RPT_WARNING, "Choose one selection domain before duplicating");
    return false;
  }
  bool invertible;
  const float4x4 inverse = math::invert(session.source_to_world, invertible);
  UNUSED_VARS(inverse);
  if (!invertible) {
    BKE_report(op.reports, RPT_WARNING, "Duplication requires an invertible object transform");
    return false;
  }
  for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    if (!selected_indices_get(*em->bm, domain, BM_ELEM_HIDDEN).is_empty()) {
      BKE_report(op.reports, RPT_WARNING, "Reveal hidden Viewer elements before duplicating");
      return false;
    }
  }
  session.hidden_verts.clear();
  session.hidden_edges.clear();
  session.hidden_faces.clear();
  const Mesh *input_mesh = viewer_mesh_get(*owner, *viewer);
  if (!input_mesh || !edit_transaction_begin(C, *tree, *viewer, proxy, session)) {
    BKE_report(op.reports, RPT_INFO, "Select elements before duplicating");
    return false;
  }
  const uint32_t proxy_uid = proxy.id.session_uid;
  const uint32_t tree_uid = session.tree_session_uid;
  const int viewer_identifier = session.viewer_identifier;
  const std::string attribute_base = ".viewer_duplicate_" +
                                     std::to_string(session.capture_node_identifier);
  session.top_attribute_name = attribute_base;
  for (int suffix = 1; input_mesh->attributes().contains(session.top_attribute_name); suffix++) {
    session.top_attribute_name = attribute_base + "_" + std::to_string(suffix);
  }
  session.duplication_preparing = true;
  if (!edit_recipe_create(C, *tree, *viewer, session, EditRecipe::Duplicate)) {
    return false;
  }
  edit_tree_tag(C, *tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), &bmain);
  if (CTX_data_main(&C) != &bmain || selection_sessions().lookup_ptr(proxy_uid) != &session ||
      find_object(bmain, proxy_uid) != &proxy || find_tree(bmain, tree_uid) != tree ||
      find_owner_space(bmain, session, true) != owner)
  {
    return false;
  }
  tree->ensure_topology_cache();
  if (tree->node_by_id(viewer_identifier) != viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, session, false))
  {
    return false;
  }
  const Mesh *result_mesh = viewer_mesh_get(*owner, *viewer);
  if (!result_mesh || result_mesh->verts_num <= session.source_signature.verts_num) {
    return false;
  }
  auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(*owner);
  if (auto *log = logs.get_main_tree_log(*viewer)) {
    log->ensure_node_warnings(bmain);
    if (const auto *capture_log = log->find_node_log(session.capture_node_identifier)) {
      if (!capture_log->warnings.is_empty()) {
        return false;
      }
    }
  }
  const auto selected = result_mesh->attributes().lookup<bool>(session.top_attribute_name,
                                                               session.edit_domain);
  Vector<int> picked;
  if (selected) {
    for (const int index : IndexRange(selected.varray.size())) {
      if (selected.varray[index]) {
        picked.append(index);
      }
    }
  }
  if (picked.size() != session.capture_indices.size() ||
      !edit_topology_proxy_replace(C, proxy, session, *result_mesh, picked))
  {
    BKE_report(
        op.reports, RPT_WARNING, "The duplicate result cannot be selected without reordering");
    return false;
  }
  session.duplication_preparing = false;
  session.duplication_armed = true;
  return true;
}

static wmOperatorStatus geometry_viewer_edit_duplicate_start(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Object *proxy = CTX_data_edit_object(C);
  const uint32_t uid = proxy ? proxy->id.session_uid : 0;
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(
        op->reports, RPT_WARNING, "Confirm or cancel the pending Viewer edit before duplicating");
    return OPERATOR_CANCELLED;
  }
  if (!event) {
    float offset[3];
    RNA_float_get_array(op->ptr, "offset", offset);
    if (!std::isfinite(offset[0]) || !std::isfinite(offset[1]) || !std::isfinite(offset[2])) {
      BKE_report(op->reports, RPT_ERROR, "Offset must contain finite values");
      return OPERATOR_CANCELLED;
    }
  }
  if (!edit_duplication_prepare(*C, *op, *proxy, *session)) {
    if (const auto *live_session = selection_sessions().lookup_ptr(uid)) {
      if (live_session->operation_node_identifier || live_session->capture_node_identifier) {
        session_end_by_uid(*C, uid);
      }
    }
    return OPERATOR_CANCELLED;
  }
  wmOperatorType *translate = WM_operatortype_find("TRANSFORM_OT_translate", false);
  PointerRNA properties = WM_operator_properties_create_ptr(translate);
  RNA_boolean_set(&properties, "release_confirm", false);
  RNA_enum_set_identifier(C, &properties, "orient_type", "GLOBAL");
  if (!event) {
    float offset[3];
    RNA_float_get_array(op->ptr, "offset", offset);
    RNA_float_set_array(&properties, "value", offset);
  }
  const wmOperatorStatus result = WM_operator_name_call_ptr(
      C,
      translate,
      event ? wm::OpCallContext::InvokeRegionWin : wm::OpCallContext::ExecDefault,
      &properties,
      event);
  WM_operator_properties_free(&properties);
  if (!(result & (OPERATOR_FINISHED | OPERATOR_RUNNING_MODAL))) {
    session_end_by_uid(*C, uid);
    return OPERATOR_CANCELLED;
  }
  /* Only the native movement child pushes the clean, single-step graph undo snapshot. */
  return OPERATOR_FINISHED;
}

static bool edit_extrusion_prepare(bContext &C,
                                   wmOperator &op,
                                   Object &proxy,
                                   GeometryViewerSelectionSession &session)
{
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, session, true);
  bNodeTree *tree = find_tree(bmain, session.tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session.viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, session, true))
  {
    BKE_report(op.reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return false;
  }
  BMEditMesh *em = BKE_editmesh_from_object(&proxy);
  if (!ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    BKE_report(op.reports, RPT_WARNING, "Use one selection domain at a time for extrusion");
    return false;
  }
  bool invertible;
  const float4x4 inverse = math::invert(session.source_to_world, invertible);
  UNUSED_VARS(inverse);
  if (!invertible) {
    BKE_report(op.reports, RPT_WARNING, "Extrusion requires an invertible object transform");
    return false;
  }
  session.extrude_individual = RNA_boolean_get(op.ptr, "individual");
  if (session.extrude_individual && em->selectmode != SCE_SELECT_FACE) {
    BKE_report(op.reports, RPT_WARNING, "Individual extrusion is only available in Face mode");
    return false;
  }
  session.hidden_verts = selected_indices_get(*em->bm, AttrDomain::Point, BM_ELEM_HIDDEN);
  session.hidden_edges = selected_indices_get(*em->bm, AttrDomain::Edge, BM_ELEM_HIDDEN);
  session.hidden_faces = selected_indices_get(*em->bm, AttrDomain::Face, BM_ELEM_HIDDEN);
  if (!edit_transaction_begin(C, *tree, *viewer, proxy, session)) {
    BKE_report(op.reports, RPT_INFO, "Select elements before extruding");
    return false;
  }
  const uint32_t proxy_uid = proxy.id.session_uid;
  const uint32_t tree_uid = session.tree_session_uid;
  const int viewer_identifier = session.viewer_identifier;
  const Mesh *input_mesh = viewer_mesh_get(*owner, *viewer);
  const std::string attribute_base = ".viewer_extrude_top_" +
                                     std::to_string(session.capture_node_identifier);
  session.top_attribute_name = attribute_base;
  if (!input_mesh) {
    return false;
  }
  for (int suffix = 1; input_mesh->attributes().contains(session.top_attribute_name); suffix++) {
    session.top_attribute_name = attribute_base + "_" + std::to_string(suffix);
  }
  session.extrusion_preparing = true;
  if (!edit_recipe_create(C, *tree, *viewer, session, EditRecipe::Extrude)) {
    return false;
  }
  edit_tree_tag(C, *tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), &bmain);
  /* Evaluation handlers may close a session, remove its owner, or delete nodes. Validate IDs
   * and live addresses before using any handle retained across evaluation. */
  if (CTX_data_main(&C) != &bmain || selection_sessions().lookup_ptr(proxy_uid) != &session ||
      find_object(bmain, proxy_uid) != &proxy || find_tree(bmain, tree_uid) != tree ||
      find_owner_space(bmain, session, true) != owner ||
      tree->node_by_id(viewer_identifier) != viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage)
  {
    return false;
  }
  const Mesh *result_mesh = viewer_mesh_get(*owner, *viewer);
  if (!result_mesh) {
    return false;
  }
  const auto result_signature = nodes::geometry_edit::mesh_signature(*result_mesh);
  const auto top = result_mesh->attributes().lookup<bool>(session.top_attribute_name,
                                                          session.edit_domain);
  Vector<int> picked;
  if (top) {
    for (const int index : IndexRange(top.varray.size())) {
      if (top.varray[index]) {
        picked.append(index);
      }
    }
  }
  if (picked.is_empty() ||
      result_signature.topology_hash == session.source_signature.topology_hash)
  {
    BKE_report(
        op.reports,
        RPT_WARNING,
        "This selection produces no new extrusion topology; select an open region boundary, "
        "edges or vertices");
    return false;
  }
  /* This is the only topology synchronization in the gesture, before native TransInfo exists.
   * Replay every offset from the original captured input, never from the last preview. */
  if (!edit_topology_proxy_replace(C, proxy, session, *result_mesh, picked)) {
    BKE_report(
        op.reports, RPT_WARNING, "The extrusion result cannot be edited without reordering");
    return false;
  }
  session.extrusion_preparing = false;
  session.extrusion_armed = true;
  return true;
}

static wmOperatorStatus geometry_viewer_edit_extrude_start(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Object *proxy = CTX_data_edit_object(C);
  const uint32_t uid = proxy ? proxy->id.session_uid : 0;
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(
        op->reports, RPT_WARNING, "Confirm or cancel the pending Viewer edit before extruding");
    return OPERATOR_CANCELLED;
  }
  if (!event) {
    float offset[3];
    RNA_float_get_array(op->ptr, "offset", offset);
    if (!std::isfinite(offset[0]) || !std::isfinite(offset[1]) || !std::isfinite(offset[2])) {
      BKE_report(op->reports, RPT_ERROR, "Offset must contain finite values");
      return OPERATOR_CANCELLED;
    }
  }
  if (!edit_extrusion_prepare(*C, *op, *proxy, *session)) {
    if (const auto *live_session = selection_sessions().lookup_ptr(uid)) {
      if (live_session->operation_node_identifier || live_session->capture_node_identifier) {
        session_end_by_uid(*C, uid);
      }
    }
    return OPERATOR_CANCELLED;
  }
  wmOperatorType *translate = WM_operatortype_find("TRANSFORM_OT_translate", false);
  PointerRNA properties = WM_operator_properties_create_ptr(translate);
  RNA_boolean_set(&properties, "release_confirm", false);
  RNA_enum_set_identifier(C, &properties, "orient_type", "GLOBAL");
  if (event && session->edit_domain == AttrDomain::Face) {
    RNA_enum_set_identifier(C, &properties, "orient_type", "NORMAL");
    const bool axes[3] = {false, false, true};
    RNA_boolean_set_array(&properties, "constraint_axis", axes);
  }
  if (!event) {
    float offset[3];
    RNA_float_get_array(op->ptr, "offset", offset);
    RNA_float_set_array(&properties, "value", offset);
  }
  const wmOperatorStatus result = WM_operator_name_call_ptr(
      C,
      translate,
      event ? wm::OpCallContext::InvokeRegionWin : wm::OpCallContext::ExecDefault,
      &properties,
      event);
  WM_operator_properties_free(&properties);
  if (!(result & (OPERATOR_FINISHED | OPERATOR_RUNNING_MODAL))) {
    session_end_by_uid(*C, uid);
    return OPERATOR_CANCELLED;
  }
  /* The native child owns the modal gesture and its single global undo step. */
  return OPERATOR_FINISHED;
}

struct ViewerLoopCutData {
  uint32_t proxy_uid = 0;
  Main *main = nullptr;
  bke::GeometrySet source_geometry;
  float4x4 source_to_world;
  uintptr_t area_token = 0, region_token = 0;
  ARegionType *region_type = nullptr;
  void *draw_handle = nullptr;
  Vector<float3> lines;
  NumInput num;
  int seed_edge = -1;
  int cuts = 1;
  float factor = 0;
  int last_mval[2] = {};
  bool valid_ring = false;
  bool sliding = false;
  std::string message;
};

static void loop_cut_draw(const bContext * /*C*/, ARegion * /*region*/, void *user_data)
{
  const auto &data = *static_cast<const ViewerLoopCutData *>(user_data);
  if (data.lines.is_empty()) {
    return;
  }
  const GPUDepthTest previous_depth = GPU_depth_test_get();
  const GPUBlend previous_blend = GPU_blend_get();
  /* Match the familiar loop-cut preselection overlay; picking itself still honors X-Ray. */
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
  immUniform2fv("viewportSize", &viewport[2]);
  immUniformThemeColor3(data.valid_ring ? TH_GIZMO_PRIMARY : TH_REDALERT);
  immUniform1f("lineWidth", 2.0f * U.pixelsize);
  immBegin(GPU_PRIM_LINES, data.lines.size());
  for (const float3 &position : data.lines) {
    immVertex3fv(pos, position);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(previous_blend);
  GPU_depth_test(previous_depth);
}

static void loop_cut_status(bContext &C, const ViewerLoopCutData &data)
{
  char text[256];
  if (!data.sliding) {
    SNPRINTF_UTF8(text,
                  IFACE_("Loop Cut | Cuts: %d | %s | Wheel/type: cuts | LMB: place | Esc: cancel"),
                  data.cuts,
                  data.message.c_str());
  }
  else {
    SNPRINTF_UTF8(
        text,
        IFACE_("Loop Slide | Factor: %.3f | %s | Shift: precision | LMB/Enter: confirm | "
               "Esc/RMB: centered cuts | Shift Esc: abort"),
        data.factor,
        data.message.c_str());
  }
  ED_area_status_text(CTX_wm_area(&C), text);
}

static void loop_cut_data_free(bContext &C, wmOperator &op)
{
  auto *data = static_cast<ViewerLoopCutData *>(op.customdata);
  if (!data) {
    return;
  }
  if (data->draw_handle) {
    ED_region_draw_cb_exit(data->region_type, data->draw_handle);
  }
  /* Only access a saved editor address after finding it in the current Main. */
  for (bScreen &screen : CTX_data_main(&C)->screens) {
    for (ScrArea &area : screen.areabase) {
      if (uintptr_t(&area) == data->area_token) {
        ED_area_status_text(&area, nullptr);
        for (ARegion &region : area.regionbase) {
          if (uintptr_t(&region) == data->region_token) {
            ED_region_tag_redraw(&region);
          }
        }
      }
    }
  }
  ED_workspace_status_text(&C, nullptr);
  MEM_delete(data);
  op.customdata = nullptr;
}

static GeometryViewerSelectionSession *loop_cut_session_get(bContext &C,
                                                            wmOperator &op,
                                                            const bool check_geometry)
{
  const auto &data = *static_cast<const ViewerLoopCutData *>(op.customdata);
  if (CTX_data_main(&C) != data.main) {
    return nullptr;
  }
  auto *session = selection_sessions().lookup_ptr(data.proxy_uid);
  Object *proxy = find_object(*data.main, data.proxy_uid);
  if (!session || session->native_operator != &op || session->native_invalidated || !proxy ||
      CTX_data_edit_object(&C) != proxy)
  {
    return nullptr;
  }
  SpaceNode *owner = find_owner_space(*data.main, *session, true);
  bNodeTree *tree = find_tree(*data.main, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, *session, check_geometry))
  {
    return nullptr;
  }
  return session;
}

static int loop_cut_hover_edge(bContext &C, const wmEvent &event)
{
  view3d_operator_needs_gpu(&C);
  ViewContext vc = em_setup_viewcontext(&C);
  copy_v2_v2_int(vc.mval, event.mval);
  Base *base = BKE_view_layer_base_find(CTX_data_view_layer(&C), vc.obedit);
  if (!base) {
    return -1;
  }
  Base *bases[] = {base};
  uint base_index;
  float distance = ED_view3d_select_dist_px();
  BMEdge *edge = EDBM_edge_find_nearest_ex(
      &vc, &distance, nullptr, false, false, nullptr, Span<Base *>(bases, 1), &base_index);
  if (!edge) {
    return -1;
  }
  BM_mesh_elem_index_ensure(vc.em->bm, BM_EDGE);
  return BM_elem_index_get(edge);
}

static bool loop_cut_update(bContext &C, wmOperator &op, const int seed_edge)
{
  auto &data = *static_cast<ViewerLoopCutData *>(op.customdata);
  auto *session = loop_cut_session_get(C, op, true);
  if (!session) {
    return false;
  }
  const Mesh &input = *data.source_geometry.get_mesh();
  const bool has_seed = input.edges().index_range().contains(seed_edge);
  IndexMaskMemory memory;
  const IndexMask seed = has_seed ? IndexMask::from_indices<int>({seed_edge}, memory) :
                                    IndexMask();
  const geometry::LoopCutSlidePreview preview = geometry::preview_loop_cut_and_slide_mesh(
      input, seed, data.cuts, data.factor);
  data.seed_edge = seed_edge;
  data.valid_ring = !preview.lines.is_empty();
  data.lines.clear();
  for (const float3 &position : preview.lines) {
    data.lines.append(math::transform_point(data.source_to_world, position));
  }
  data.message = data.valid_ring ? IFACE_("Quad ring") :
                 has_seed        ? IFACE_("No quad ring, non-manifold or twisted ring") :
                                   IFACE_("Hover an edge");
  if (!data.valid_ring && has_seed) {
    const int2 edge = input.edges()[seed_edge];
    for (const int vert : {edge[0], edge[1]}) {
      data.lines.append(math::transform_point(data.source_to_world, input.vert_positions()[vert]));
    }
  }
  if (!data.valid_ring) {
    /* No hovered ring is a normal placement state, not an empty durable capture. Keep the last
     * valid candidate (if any) for validation, but hide its overlay and disallow placement until
     * another valid ring is hovered. Cut-count changes remain in data until that next update. */
    return true;
  }
  bNodeTree &tree = *find_tree(*data.main, session->tree_session_uid);
  SpaceNode &owner = *find_owner_space(*data.main, *session, true);
  bNode &viewer = *tree.node_by_id(session->viewer_identifier);
  Object &proxy = *find_object(*data.main, data.proxy_uid);
  session->native_in_callback = session->loop_cut_preparing = true;
  if (!session->operation_node_identifier) {
    if (!edit_transaction_begin(C, tree, viewer, proxy, *session, seed_edge)) {
      session->native_in_callback = session->loop_cut_preparing = false;
      return false;
    }
    const std::string base = ".viewer_cut_loops_" +
                             std::to_string(session->capture_node_identifier);
    session->top_attribute_name = base;
    for (int suffix = 1; input.attributes().contains(session->top_attribute_name); suffix++) {
      session->top_attribute_name = base + "_" + std::to_string(suffix);
    }
    if (!edit_recipe_create(C, tree, viewer, *session, EditRecipe::LoopCut)) {
      session->native_in_callback = session->loop_cut_preparing = false;
      return false;
    }
  }
  session->picked_indices.clear();
  session->point_indices.clear();
  if (data.valid_ring) {
    session->picked_indices.append(seed_edge);
    const int2 verts = input.edges()[seed_edge];
    session->point_indices = {std::min(verts[0], verts[1]), std::max(verts[0], verts[1])};
  }
  tree.ensure_topology_cache();
  bNode &capture = *tree.node_by_id(session->capture_node_identifier);
  nodes::geometry_edit::capture_initialize(
      *static_cast<NodeGeometryCapturedSelection *>(capture.storage),
      session->source_signature,
      AttrDomain::Edge,
      session->picked_indices,
      session->point_indices);
  capture_indices_text_update(capture);
  BKE_ntree_update_tag_node_property(&tree, &capture);
  bNode &operation = *tree.node_by_id(session->operation_node_identifier);
  bke::node_find_socket(operation, SOCK_IN, "Cuts"_ustr)
      ->default_value_typed<bNodeSocketValueInt>()
      ->value = session->loop_cut_cuts = data.cuts;
  bke::node_find_socket(operation, SOCK_IN, "Factor"_ustr)
      ->default_value_typed<bNodeSocketValueFloat>()
      ->value = session->loop_cut_factor = data.factor;
  BKE_ntree_update_tag_node_property(&tree, &operation);
  edit_tree_tag(C, tree);
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), data.main);
  /* Evaluation may run arbitrary handlers. No stale node/editor/session pointer is dereferenced.
   */
  auto *live = selection_sessions().lookup_ptr(data.proxy_uid);
  if (live == session) {
    live->native_in_callback = live->loop_cut_preparing = false;
  }
  session = loop_cut_session_get(C, op, false);
  if (!session || find_tree(*data.main, session->tree_session_uid) != &tree ||
      find_owner_space(*data.main, *session, true) != &owner ||
      tree.node_by_id(session->viewer_identifier) != &viewer)
  {
    return false;
  }
  const Mesh *result = viewer_mesh_get(owner, viewer);
  if (!result) {
    return false;
  }
  const auto loops = result->attributes().lookup<bool>(session->top_attribute_name,
                                                       AttrDomain::Edge);
  int selected = 0;
  if (loops) {
    for (const int index : IndexRange(loops.varray.size())) {
      selected += loops.varray[index];
    }
  }
  if (selected != preview.lines.size() / 2) {
    return false;
  }
  session->loop_cut_preview_signature = nodes::geometry_edit::mesh_signature(*result);
  session->generation++;
  return loop_cut_session_get(C, op, true) != nullptr;
}

static bool loop_cut_finish(bContext &C,
                            wmOperator &op,
                            const bool commit,
                            const bool abort_all = false,
                            const bool centered = false)
{
  auto &data = *static_cast<ViewerLoopCutData *>(op.customdata);
  const uint32_t uid = data.proxy_uid;
  if (centered) {
    data.factor = 0;
    if (!loop_cut_update(C, op, data.seed_edge)) {
      if (auto *session = selection_sessions().lookup_ptr(uid)) {
        session->native_invalidated = true;
      }
    }
  }
  auto *session = loop_cut_session_get(C, op, true);
  if (commit && session && data.valid_ring && session->operation_node_identifier) {
    SpaceNode &owner = *find_owner_space(*data.main, *session, true);
    bNodeTree &tree = *find_tree(*data.main, session->tree_session_uid);
    bNode &viewer = *tree.node_by_id(session->viewer_identifier);
    const Mesh &result = *viewer_mesh_get(owner, viewer);
    const auto loops = result.attributes().lookup<bool>(session->top_attribute_name,
                                                        AttrDomain::Edge);
    GeometryViewerEditResume resume{
        *session, session->operation_node_identifier, Vector<float3>(result.vert_positions())};
    resume.session.proxy_signature = nodes::geometry_edit::mesh_signature(result);
    resume.session.positions = resume.positions;
    resume.session.verts_num = result.verts_num;
    resume.session.edges_num = result.edges_num;
    resume.session.faces_num = result.faces_num;
    resume.session.edit_domain = AttrDomain::Edge;
    resume.session.picked_indices.clear();
    resume.session.point_indices.clear();
    if (loops) {
      for (const int index : IndexRange(loops.varray.size())) {
        if (loops.varray[index]) {
          resume.session.picked_indices.append(index);
        }
      }
    }
    if (!resume.session.picked_indices.is_empty()) {
      resume.session.active_element_type = BM_EDGE;
      resume.session.active_element_index = resume.session.picked_indices[0];
      resume.session.loop_cut_sliding = false;
      session->native_operator = nullptr;
      loop_cut_data_free(C, op);
      edit_transaction_commit(C, owner, tree, viewer, *session, std::move(resume));
      return true;
    }
  }
  /* Ordinary placement cancel restores the untouched proxy and its selection. Forced cancel,
   * frame/context changes and Shift Esc also retire the entire runtime session. */
  auto *live = selection_sessions().lookup_ptr(uid);
  const bool end_session = !session || abort_all || (live && live->native_end_requested);
  if (live && live->native_operator == &op) {
    live->native_operator = nullptr;
    live->native_in_callback = live->loop_cut_preparing = false;
    edit_transaction_rollback(C, *live);
    live->recipe = EditRecipe::Offset;
    live->loop_cut_sliding = false;
  }
  loop_cut_data_free(C, op);
  if (end_session) {
    session_end_by_uid(C, uid);
  }
  if (commit) {
    BKE_report(op.reports, RPT_WARNING, "Loop cut cancelled: its Viewer input or context changed");
  }
  return false;
}

static wmOperatorStatus geometry_viewer_edit_loop_cut_start(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  Object *proxy = CTX_data_edit_object(C);
  auto *session = proxy ? selection_sessions().lookup_ptr(proxy->id.session_uid) : nullptr;
  if (!session || !session->editing || session->native_operator) {
    return OPERATOR_CANCELLED;
  }
  if (session->operation_node_identifier) {
    BKE_report(
        op->reports, RPT_WARNING, "Confirm or cancel the pending Viewer edit before cutting");
    return OPERATOR_CANCELLED;
  }
  Main &bmain = *CTX_data_main(C);
  SpaceNode *owner = find_owner_space(bmain, *session, true);
  bNodeTree *tree = find_tree(bmain, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(*C, *owner, *viewer, *session, true))
  {
    session_end_by_uid(*C, proxy->id.session_uid);
    BKE_report(op->reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return OPERATOR_CANCELLED;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  if (!ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    BKE_report(op->reports, RPT_WARNING, "Use one selection domain at a time for Viewer loop cut");
    return OPERATOR_CANCELLED;
  }
  /* Local hide flags have no stable correspondence after subdivision. Do not guess new indices
   * or cut an invisible region; visibility-attribute propagation is a separately scoped tool. */
  for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    if (!selected_indices_get(*em->bm, domain, BM_ELEM_HIDDEN).is_empty()) {
      BKE_report(op->reports, RPT_WARNING, "Reveal hidden elements before using Viewer loop cut");
      return OPERATOR_CANCELLED;
    }
  }
  if (!std::isfinite(RNA_float_get(op->ptr, "factor"))) {
    BKE_report(op->reports, RPT_ERROR, "Factor must be finite");
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<ViewerLoopCutData>(__func__);
  op->customdata = data;
  data->proxy_uid = proxy->id.session_uid;
  data->main = &bmain;
  data->source_geometry = bke::GeometrySet::from_mesh(
      BKE_mesh_copy_for_eval(*viewer_mesh_get(*owner, *viewer)));
  data->source_to_world = session->source_to_world;
  data->cuts = std::clamp(RNA_int_get(op->ptr, "cuts"), 1, 100);
  data->factor = event ? 0 : std::clamp(RNA_float_get(op->ptr, "factor"), -1.0f, 1.0f);
  initNumInput(&data->num);
  data->num.idx_max = 0;
  data->num.val_flag[0] = NUM_NO_NEGATIVE | NUM_NO_ZERO | NUM_NO_FRACTION;
  session->recipe = EditRecipe::LoopCut;
  session->loop_cut_preview_signature = session->source_signature;
  session->native_operator = op;
  session->native_invalidated = session->native_end_requested = false;
  session->hidden_verts.clear();
  session->hidden_edges.clear();
  session->hidden_faces.clear();
  const int seed = event ? loop_cut_hover_edge(*C, *event) : RNA_int_get(op->ptr, "edge_index");
  if (!loop_cut_update(*C, *op, seed)) {
    loop_cut_finish(*C, *op, false, true);
    return OPERATOR_CANCELLED;
  }
  if (!event) {
    if (!data->valid_ring) {
      BKE_report(op->reports, RPT_WARNING, "The seed edge has no valid quad ring");
      loop_cut_finish(*C, *op, false);
      return OPERATOR_CANCELLED;
    }
    return loop_cut_finish(*C, *op, true) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }
  ARegion *region = CTX_wm_region(C);
  data->area_token = uintptr_t(CTX_wm_area(C));
  data->region_token = uintptr_t(region);
  data->region_type = region->runtime->type;
  if (data->region_type) {
    data->draw_handle = ED_region_draw_cb_activate(
        data->region_type, loop_cut_draw, data, REGION_DRAW_POST_VIEW);
  }
  op->flag |= OP_IS_MODAL_CURSOR_REGION;
  WM_event_add_modal_handler(C, op);
  loop_cut_status(*C, *data);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus geometry_viewer_edit_loop_cut_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  auto &data = *static_cast<ViewerLoopCutData *>(op->customdata);
  if (uintptr_t(CTX_wm_area(C)) != data.area_token ||
      uintptr_t(CTX_wm_region(C)) != data.region_token || !loop_cut_session_get(*C, *op, true))
  {
    loop_cut_finish(*C, *op, false, true);
    return OPERATOR_CANCELLED;
  }
  if (ISMOUSE(event->type) && !BLI_rcti_isect_pt_v(&CTX_wm_region(C)->winrct, event->xy)) {
    /* Keep the explicitly labeled Viewer abort button and other editor UI usable. */
    return OPERATOR_PASS_THROUGH;
  }
  if (event->val == KM_PRESS && ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
    const bool keep_cuts = data.sliding &&
                           !(event->type == EVT_ESCKEY && event->modifier & KM_SHIFT);
    return loop_cut_finish(*C, *op, keep_cuts, !keep_cuts && data.sliding, keep_cuts) ?
               OPERATOR_FINISHED :
               OPERATOR_CANCELLED;
  }
  if (event->val == KM_PRESS && ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)) {
    if (!data.valid_ring) {
      BKE_report(op->reports, RPT_INFO, "Hover a visible edge with a valid quad ring");
      return OPERATOR_RUNNING_MODAL;
    }
    if (data.sliding) {
      return loop_cut_finish(*C, *op, true) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    data.sliding = true;
    data.message = IFACE_("Move along the seed edge or type Factor");
    selection_sessions().lookup_ptr(data.proxy_uid)->loop_cut_sliding = true;
    copy_v2_v2_int(data.last_mval, event->mval);
    initNumInput(&data.num);
    data.num.idx_max = 0;
    data.num.val_inc[0] = 0.1f;
    loop_cut_status(*C, data);
    return OPERATOR_RUNNING_MODAL;
  }
  bool update = false;
  int seed = data.seed_edge;
  if (event->val == KM_PRESS && handleNumInput(C, &data.num, event)) {
    float value = data.sliding ? data.factor : float(data.cuts);
    applyNumInput(&data.num, &value);
    if (std::isfinite(value)) {
      if (data.sliding) {
        data.factor = std::clamp(value, -1.0f, 1.0f);
      }
      else {
        data.cuts = int(std::clamp(value, 1.0f, 100.0f));
      }
      update = true;
    }
  }
  else if (event->val == KM_PRESS && !data.sliding &&
           ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE, EVT_PAGEUPKEY, EVT_PAGEDOWNKEY))
  {
    data.cuts = std::clamp(
        data.cuts + (ELEM(event->type, WHEELUPMOUSE, EVT_PAGEUPKEY) ? 1 : -1), 1, 100);
    initNumInput(&data.num);
    data.num.idx_max = 0;
    data.num.val_flag[0] = NUM_NO_NEGATIVE | NUM_NO_ZERO | NUM_NO_FRACTION;
    update = true;
  }
  else if (event->type == MOUSEMOVE) {
    if (!data.sliding) {
      seed = loop_cut_hover_edge(*C, *event);
      update = seed != data.seed_edge;
    }
    else if (!hasNumInput(&data.num)) {
      const Mesh &input = *data.source_geometry.get_mesh();
      const int2 verts = input.edges()[seed];
      float2 a, b;
      const float3 world_a = math::transform_point(data.source_to_world,
                                                   input.vert_positions()[verts[0]]);
      const float3 world_b = math::transform_point(data.source_to_world,
                                                   input.vert_positions()[verts[1]]);
      ARegion *region = CTX_wm_region(C);
      data.message = IFACE_("Edge is end-on; type Factor");
      if (ED_view3d_project_float_global(region, world_a, a, V3D_PROJ_TEST_CLIP_NEAR) ==
              V3D_PROJ_RET_OK &&
          ED_view3d_project_float_global(region, world_b, b, V3D_PROJ_TEST_CLIP_NEAR) ==
              V3D_PROJ_RET_OK)
      {
        const float2 direction = b - a;
        const float length_squared = math::length_squared(direction);
        if (length_squared > 1.0f) {
          data.message = IFACE_("Move along the seed edge or type Factor");
          const float2 delta(event->mval[0] - data.last_mval[0],
                             event->mval[1] - data.last_mval[1]);
          const float precision = event->modifier & KM_SHIFT ? 0.1f : 1.0f;
          data.factor = std::clamp(data.factor + precision * (data.cuts + 1) *
                                                     math::dot(delta, direction) / length_squared,
                                   -1.0f,
                                   1.0f);
          update = true;
        }
      }
    }
    copy_v2_v2_int(data.last_mval, event->mval);
  }
  else if (ELEM(event->type, MIDDLEMOUSE, NDOF_MOTION, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    return OPERATOR_PASS_THROUGH;
  }
  if (update && !loop_cut_update(*C, *op, seed)) {
    loop_cut_finish(*C, *op, false, true);
    return OPERATOR_CANCELLED;
  }
  loop_cut_status(*C, data);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static void node_identifier_property_define(wmOperatorType &operator_type)
{
  PropertyRNA *property = RNA_def_int(operator_type.srna,
                                      "node_identifier",
                                      0,
                                      INT_MIN,
                                      INT_MAX,
                                      "Node Identifier",
                                      "Internal identifier of the Viewer node",
                                      INT_MIN,
                                      INT_MAX);
  RNA_def_property_flag(property, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace

bool geometry_viewer_selection_is_active()
{
  return !selection_sessions().is_empty();
}

bool geometry_viewer_selection_owns_edit_mesh(const bContext &C)
{
  if (selection_sessions().is_empty()) {
    return false;
  }
  /* An Edit Mode exit can push undo before the notifier retires its still-active proxy. */
  const Object *object = CTX_data_edit_object(&C);
  if (!object) {
    object = CTX_data_active_object(&C);
  }
  return object && selection_sessions().contains(object->id.session_uid);
}

bool geometry_viewer_selection_skip_operator_undo(const bContext &C,
                                                  const wmOperatorType &operator_type)
{
  const SpaceNode *snode = CTX_wm_space_node(&C);
  const Object *object = CTX_data_edit_object(&C);
  const auto *session = object ? selection_sessions().lookup_ptr(object->id.session_uid) : nullptr;
  if (!snode || !session || uintptr_t(snode) != session->owner_space_token || !snode->edittree ||
      snode->edittree->id.session_uid != session->tree_session_uid)
  {
    return false;
  }
  /* Node selection is UI state, not a graph edit. Do not globally snapshot its temporary proxy.
   * In particular, Select Link Viewer is NOT included: it can create and rewire nodes. */
  return ELEM(StringRef(operator_type.idname),
              "NODE_OT_select",
              "NODE_OT_select_all",
              "NODE_OT_select_box",
              "NODE_OT_select_circle",
              "NODE_OT_select_lasso",
              "NODE_OT_select_grouped",
              "NODE_OT_select_linked_to",
              "NODE_OT_select_linked_from",
              "NODE_OT_select_same_type_step");
}

static std::optional<GeometryViewerEditResume> node_layout_resume_get(bContext &C)
{
  const SpaceNode *snode = CTX_wm_space_node(&C);
  Object *proxy = CTX_data_edit_object(&C);
  auto *session = proxy ? selection_sessions().lookup_ptr(proxy->id.session_uid) : nullptr;
  if (!snode || !session || !session->editing || session->native_operator ||
      session->operation_node_identifier || uintptr_t(snode) != session->owner_space_token ||
      !snode->edittree || snode->edittree->id.session_uid != session->tree_session_uid)
  {
    return std::nullopt;
  }
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, *session, true);
  bNode *viewer = snode->edittree->node_by_id(session->viewer_identifier);
  if (!owner || !viewer || !edit_session_validate(C, *owner, *viewer, *session, true)) {
    return std::nullopt;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  const auto domain = selection_domain_from_mode(em->selectmode);
  if (!domain) {
    return std::nullopt;
  }
  BM_mesh_elem_index_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);
  GeometryViewerEditResume resume{*session, session->source_node_identifier, session->positions};
  resume.node_layout = true;
  auto &saved = resume.session;
  saved.edit_domain = *domain;
  saved.picked_indices = selected_indices_get(*em->bm, *domain);
  saved.point_indices = affected_points_get(*em->bm, *domain);
  saved.hidden_verts = selected_indices_get(*em->bm, AttrDomain::Point, BM_ELEM_HIDDEN);
  saved.hidden_edges = selected_indices_get(*em->bm, AttrDomain::Edge, BM_ELEM_HIDDEN);
  saved.hidden_faces = selected_indices_get(*em->bm, AttrDomain::Face, BM_ELEM_HIDDEN);
  saved.active_element_type = 0;
  saved.active_element_index = -1;
  BMEditSelection active{};
  if (BM_select_history_active_get(em->bm, &active)) {
    saved.active_element_type = active.htype;
    saved.active_element_index = BM_elem_index_get(active.ele);
  }
  else if (*domain == AttrDomain::Face) {
    if (BMFace *face = BM_mesh_active_face_get(em->bm, false, true)) {
      saved.active_element_type = BM_FACE;
      saved.active_element_index = BM_elem_index_get(face);
    }
  }
  return resume;
}

bool geometry_viewer_node_layout_undo_push_prepare(bContext &C,
                                                   const wmOperatorType &operator_type)
{
  if (!ELEM(StringRef(operator_type.idname),
            "TRANSFORM_OT_translate",
            "TRANSFORM_OT_resize",
            "TRANSFORM_OT_rotate",
            "TRANSFORM_OT_transform",
            "NODE_OT_translate_attach",
            "NODE_OT_attach"))
  {
    return false;
  }
  auto resume = node_layout_resume_get(C);
  if (!resume) {
    return false;
  }
  geometry_viewer_selection_end_all(C);
  edit_resume() = std::move(resume);
  return true;
}

void geometry_viewer_node_layout_undo_begin(bContext &C, const bool layout_step)
{
  layout_undo_resume() = layout_step ? node_layout_resume_get(C) : std::nullopt;
}

void geometry_viewer_node_layout_undo_end(bContext &C)
{
  UNUSED_VARS(C);
  if (layout_undo_resume()) {
    edit_resume() = std::move(layout_undo_resume());
    layout_undo_resume().reset();
  }
}

bool geometry_viewer_edit_operator_poll(bContext &C, const wmOperatorType &operator_type)
{
  if (edit_resume() && StringRef(operator_type.idname).startswith("TRANSFORM_OT_")) {
    CTX_wm_operator_poll_msg_set(&C, "The Viewer is refreshing the next editing stage");
    return false;
  }
  if (selection_sessions().is_empty()) {
    return true;
  }
  const Object *object = CTX_data_edit_object(&C);
  const auto *session = object ? selection_sessions().lookup_ptr(object->id.session_uid) : nullptr;
  if (!session || !session->editing) {
    return true;
  }
  const StringRef idname(operator_type.idname);
  bool allowed = true;
  if (idname.startswith("MESH_OT_")) {
    allowed = !session->native_operator &&
              (idname.startswith("MESH_OT_select") || idname.startswith("MESH_OT_loop_select") ||
               idname == "MESH_OT_loop_multi_select" ||
               idname.startswith("MESH_OT_edgering_select") ||
               idname.startswith("MESH_OT_shortest_path") || idname == "MESH_OT_hide" ||
               idname == "MESH_OT_reveal");
  }
  else if (idname.startswith("TRANSFORM_OT_")) {
    allowed = !session->native_operator &&
              (!CTX_wm_space_node(&C) || !session->operation_node_identifier) &&
              ELEM(idname,
                   "TRANSFORM_OT_translate",
                   "TRANSFORM_OT_resize",
                   "TRANSFORM_OT_rotate",
                   "TRANSFORM_OT_from_gizmo",
                   "TRANSFORM_OT_transform");
  }
  else if (idname.startswith("OBJECT_OT_")) {
    allowed = !session->native_operator &&
              ELEM(idname, "OBJECT_OT_mode_set", "OBJECT_OT_editmode_toggle");
  }
  else if (idname.startswith("NODE_OT_")) {
    allowed = idname.startswith("NODE_OT_geometry_viewer_") ||
              idname.startswith("NODE_OT_select") || idname.startswith("NODE_OT_view") ||
              idname == "NODE_OT_find_node" ||
              (!session->native_operator && !session->operation_node_identifier &&
               ELEM(
                   idname, "NODE_OT_translate_attach", "NODE_OT_attach", "NODE_OT_insert_offset"));
  }
  if (!allowed) {
    CTX_wm_operator_poll_msg_set(
        &C,
        "This edit would only change the temporary Viewer mesh. Use "
        "Move, Scale, Rotate, E Extrude, Ctrl R Loop Cut or X "
        "Delete/Dissolve, M Merge, Shift D Duplicate, Ctrl T Triangulate or Ctrl F Face Tools, "
        "or disable Viewer editing first");
  }
  return allowed;
}

bool geometry_viewer_editing_owns_edit_mesh(const bContext &C)
{
  const Object *object = CTX_data_edit_object(&C);
  const auto *session = object ? selection_sessions().lookup_ptr(object->id.session_uid) : nullptr;
  return session && session->editing;
}

bool geometry_viewer_edit_has_pending(const bNode &viewer)
{
  if (!viewer.storage) {
    return false;
  }
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer.storage);
  const auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
  return session && (session->operation_node_identifier != 0 || session->native_operator);
}

bool geometry_viewer_edit_transform_is_running(const bNode &viewer)
{
  if (!viewer.storage) {
    return false;
  }
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer.storage);
  const auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
  return session && session->native_operator;
}

bool geometry_viewer_edit_extrusion_is_pending(const bNode &viewer)
{
  if (!viewer.storage) {
    return false;
  }
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer.storage);
  const auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
  return session && session->recipe == EditRecipe::Extrude && session->operation_node_identifier;
}

bool geometry_viewer_edit_duplication_is_pending(const bNode &viewer)
{
  if (!viewer.storage) {
    return false;
  }
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer.storage);
  const auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
  return session && session->recipe == EditRecipe::Duplicate && session->operation_node_identifier;
}

GeometryViewerLoopCutStage geometry_viewer_edit_loop_cut_stage(const bNode &viewer)
{
  if (viewer.storage) {
    const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer.storage);
    const auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid);
    if (session && session->native_operator && session->recipe == EditRecipe::LoopCut) {
      return session->loop_cut_sliding ? GeometryViewerLoopCutStage::Slide :
                                         GeometryViewerLoopCutStage::Cuts;
    }
  }
  return GeometryViewerLoopCutStage::None;
}

bool geometry_viewer_edit_transform_begin(bContext &C,
                                          wmOperator &op,
                                          const GeometryViewerTransform kind,
                                          bool &r_topology_move)
{
  r_topology_move = false;
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), CTX_data_main(&C));
  Object *proxy = CTX_data_edit_object(&C);
  auto *session = proxy ? selection_sessions().lookup_ptr(proxy->id.session_uid) : nullptr;
  if (!session || !session->editing || session->native_operator) {
    return false;
  }
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, *session, true);
  bNodeTree *tree = find_tree(bmain, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, *session, true))
  {
    session_end_by_uid(C, proxy->id.session_uid);
    BKE_report(op.reports, RPT_WARNING, "Viewer input changed; enable editing again");
    return false;
  }
  const bool extrusion = session->extrusion_armed && session->recipe == EditRecipe::Extrude;
  const bool duplication = session->duplication_armed && session->recipe == EditRecipe::Duplicate;
  const bool topology_move = extrusion || duplication;
  if (session->operation_node_identifier && !topology_move) {
    BKE_report(op.reports,
               RPT_WARNING,
               "Finish or cancel the current viewport operation before starting another");
    return false;
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  if (!ELEM(em->selectmode, SCE_SELECT_VERTEX, SCE_SELECT_EDGE, SCE_SELECT_FACE)) {
    BKE_report(
        op.reports, RPT_WARNING, "Use one selection domain at a time for Viewer transforms");
    return false;
  }
  bool invertible;
  const float4x4 inverse = math::invert(session->source_to_world, invertible);
  UNUSED_VARS(inverse);
  if (!invertible) {
    BKE_report(
        op.reports, RPT_WARNING, "Viewer transforms require an invertible object transform");
    return false;
  }
  session->hidden_verts = selected_indices_get(*em->bm, AttrDomain::Point, BM_ELEM_HIDDEN);
  session->hidden_edges = selected_indices_get(*em->bm, AttrDomain::Edge, BM_ELEM_HIDDEN);
  session->hidden_faces = selected_indices_get(*em->bm, AttrDomain::Face, BM_ELEM_HIDDEN);
  const bool reuse_move = !topology_move && kind == GeometryViewerTransform::Move &&
                          edit_move_batch_begin(*tree, *proxy, *session);
  if (!topology_move && !reuse_move &&
      !edit_transaction_begin(C, *tree, *viewer, *proxy, *session))
  {
    BKE_report(op.reports, RPT_INFO, "Select mesh elements before using a Viewer transform");
    return false;
  }
  session->native_operator = &op;
  session->transform_kind = kind;
  session->native_invalidated = session->native_end_requested = false;
  session->extrusion_armed = false;
  session->duplication_armed = false;
  r_topology_move = topology_move;
  return true;
}

bool geometry_viewer_edit_transform_update(bContext &C,
                                           const GeometryViewerTransform kind,
                                           const float4x4 &world_transform,
                                           const float3 &world_pivot,
                                           const int pivot_mode,
                                           const bool pivot_override)
{
  Object *proxy = CTX_data_edit_object(&C);
  auto *session = proxy ? selection_sessions().lookup_ptr(proxy->id.session_uid) : nullptr;
  if (!session || !session->native_operator || session->native_invalidated) {
    return false;
  }
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, *session, true);
  bNodeTree *tree = find_tree(bmain, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (!owner || owner->edittree != tree || !viewer || viewer->type_legacy != GEO_NODE_VIEWER ||
      !viewer->storage || !edit_session_validate(C, *owner, *viewer, *session, false))
  {
    session->native_invalidated = true;
    return false;
  }
  for (const int column : IndexRange(4)) {
    for (const int row : IndexRange(4)) {
      if (!std::isfinite(world_transform[column][row])) {
        session->native_invalidated = true;
        return false;
      }
    }
  }
  const float4x4 inverse = math::invert(session->source_to_world);
  session->local_transform = inverse * world_transform * session->source_to_world;
  session->transform_kind = kind;
  if (kind == GeometryViewerTransform::Rotate) {
    const float3x3 world_rotation(world_transform);
    session->rotation_euler = float3(math::to_euler(world_rotation));
    const float3x3 replay = math::from_rotation<float3x3>(math::EulerXYZ(session->rotation_euler));
    for (const int column : IndexRange(3)) {
      for (const int row : IndexRange(3)) {
        if (std::abs(replay[column][row] - world_rotation[column][row]) > 1.0e-5f) {
          session->native_invalidated = true;
          return false;
        }
      }
    }
  }
  if (kind == GeometryViewerTransform::Move) {
    /* Avoid inverse(M)*M roundoff in the identity linear part of a pure translation. */
    session->local_transform = float4x4::identity();
    session->local_transform.location() = math::transform_direction(inverse,
                                                                    world_transform.location());
  }
  BMEditMesh *em = BKE_editmesh_from_object(proxy);
  BM_mesh_elem_table_ensure(em->bm, BM_VERT);
  const Vector<float3> expected = edit_result_positions(*session);
  for (const int index : expected.index_range()) {
    if (!edit_position_matches(float3(BM_vert_at_index(em->bm, index)->co), expected[index])) {
      /* Reject proportional editing, projection or another non-affine effect instead of
       * recording a graph that does not reproduce what the user saw. */
      session->native_invalidated = true;
      return false;
    }
  }
  session->native_in_callback = true;
  if (session->reusing_move && kind != GeometryViewerTransform::Move) {
    /* A native G gesture can switch to S/R. Leave the earlier committed translation intact and
     * author the new recipe on that result; never let recipe replacement delete borrowed nodes.
     */
    const float4x4 local_transform = session->local_transform;
    const float3 rotation_euler = session->rotation_euler;
    edit_transaction_rollback(C, *session);
    if (!session->move_batch || !edit_transaction_begin(C, *tree, *viewer, *proxy, *session)) {
      session->native_in_callback = false;
      session->native_invalidated = true;
      return false;
    }
    session->local_transform = local_transform;
    session->rotation_euler = rotation_euler;
    session->transform_kind = kind;
  }
  const float3 local_pivot = math::transform_point(inverse, world_pivot);
  if (!session->reusing_move) {
    bNode &capture = *tree->node_by_id(session->capture_node_identifier);
    auto &data = *static_cast<NodeGeometryCapturedSelection *>(capture.storage);
    data.pivot_mode = int8_t(nodes::geometry_edit::CapturePivot::Fixed);
    copy_v3_v3(data.pivot, local_pivot);
    if (!pivot_override && pivot_mode == V3D_AROUND_CENTER_MEDIAN) {
      data.pivot_mode = int8_t(nodes::geometry_edit::CapturePivot::Median);
    }
    else if (!pivot_override && pivot_mode == V3D_AROUND_ACTIVE &&
             session->active_element_index >= 0)
    {
      data.pivot_mode = int8_t(nodes::geometry_edit::CapturePivot::Active);
      /* Duplication's deterministic active copy corresponds to the first captured source element.
       * Its result index must never be stored as an active index on the original capture geometry.
       */
      data.active_index = session->recipe == EditRecipe::Duplicate ? session->capture_indices[0] :
                                                                     session->active_element_index;
      data.active_domain = int8_t(session->recipe == EditRecipe::Duplicate ? session->edit_domain :
                                  session->active_element_type == BM_VERT  ? AttrDomain::Point :
                                  session->active_element_type == BM_EDGE  ? AttrDomain::Edge :
                                                                             AttrDomain::Face);
    }
    BKE_ntree_update_tag_node_property(tree, &capture);
  }
  const bool updated = edit_recipe_values_update(C, *tree, *viewer, *session, kind, local_pivot);
  session->native_in_callback = false;
  if (!updated) {
    session->native_invalidated = true;
  }
  return updated && !session->native_invalidated;
}

bool geometry_viewer_edit_transform_finish(bContext &C,
                                           wmOperator &op,
                                           const bool confirmed,
                                           const bool abort_operation)
{
  uint32_t uid = 0;
  for (const auto item : selection_sessions().items()) {
    if (item.value.native_operator == &op) {
      uid = item.key;
      break;
    }
  }
  auto *session = selection_sessions().lookup_ptr(uid);
  if (!session) {
    return false;
  }
  /* TransInfo is already destroyed. Evaluation may invalidate the session but must not cancel
   * this operator recursively while its exit callback is running. */
  session->native_in_callback = true;
  const bool topology_move = ELEM(session->recipe, EditRecipe::Extrude, EditRecipe::Duplicate);
  session->native_invalidated |= abort_operation;
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, *session, true);
  bNodeTree *tree = find_tree(bmain, session->tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  if (topology_move && !confirmed && !session->native_invalidated && tree && viewer) {
    /* Normal Esc/RMB reverts movement but keeps the topology. Forced cancellation aborts it. */
    session->local_transform = float4x4::identity();
    if (!edit_recipe_values_update(
            C, *tree, *viewer, *session, GeometryViewerTransform::Move, float3(0)))
    {
      session->native_invalidated = true;
    }
  }
  if ((confirmed || topology_move) && !session->native_invalidated) {
    BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), CTX_data_main(&C));
  }
  owner = find_owner_space(bmain, *session, true);
  tree = find_tree(bmain, session->tree_session_uid);
  viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
  const bool valid = !session->native_invalidated && owner && owner->edittree == tree && viewer &&
                     viewer->type_legacy == GEO_NODE_VIEWER && viewer->storage &&
                     edit_session_validate(
                         C, *owner, *viewer, *session, confirmed || topology_move);
  bool changed = topology_move;
  /* Invalid topology cannot safely be indexed with the frozen selection. */
  if (valid) {
    if (Object *proxy = find_object(bmain, uid)) {
      if (BMEditMesh *em = BKE_editmesh_from_object(proxy)) {
        BM_mesh_elem_table_ensure(em->bm, BM_VERT);
        for (const int index : session->point_indices) {
          changed |= !equals_v3v3(BM_vert_at_index(em->bm, index)->co, session->positions[index]);
        }
      }
    }
  }
  if ((!confirmed && !topology_move) || !valid || !changed) {
    session->native_in_callback = false;
    session->native_operator = nullptr;
    edit_transaction_rollback(C, *session);
    if (!valid || session->native_end_requested) {
      session_end_by_uid(C, uid);
    }
    else if (Object *proxy = find_object(bmain, uid)) {
      edit_proxy_positions_update(C, *proxy, *session);
    }
    if (confirmed && !valid) {
      BKE_report(op.reports,
                 RPT_WARNING,
                 "Viewer transform cancelled: its input changed or the gesture is not supported");
    }
    return false;
  }
  if (session->transform_kind == GeometryViewerTransform::Rotate) {
    const uint32_t tree_uid = session->tree_session_uid;
    const int viewer_identifier = session->viewer_identifier;
    session->rotation_group_preparing = true;
    bool grouped = edit_rotation_group_create(C, *tree, *viewer, *session);
    if (grouped) {
      BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(&C), &bmain);
      /* Evaluation can run handlers that replace Main, remove nodes, or retire the session. */
      if (CTX_data_main(&C) != &bmain || selection_sessions().lookup_ptr(uid) != session ||
          find_tree(bmain, tree_uid) != tree || find_owner_space(bmain, *session, true) != owner)
      {
        if (auto *live = selection_sessions().lookup_ptr(uid)) {
          if (live->native_operator == &op) {
            live->native_in_callback = false;
            live->native_operator = nullptr;
            session_end_by_uid(C, uid);
          }
        }
        return false;
      }
      tree->ensure_topology_cache();
      grouped = tree->node_by_id(viewer_identifier) == viewer && !session->native_invalidated &&
                edit_session_validate(C, *owner, *viewer, *session, true);
      auto logs = nodes::eval_log::NodesEvalLog::get_contextual_tree_logs(*owner);
      if (auto *log = logs.get_main_tree_log(*viewer)) {
        log->ensure_node_warnings(bmain);
        if (const auto *group_log = log->find_node_log(session->rotation_group_node_identifier)) {
          grouped &= group_log->warnings.is_empty();
        }
      }
      else {
        grouped = false;
      }
    }
    if (!grouped) {
      session->native_in_callback = false;
      session->native_operator = nullptr;
      edit_transaction_rollback(C, *session);
      session_end_by_uid(C, uid);
      BKE_report(
          op.reports, RPT_WARNING, "Set Rotation could not be recorded safely; edit cancelled");
      return false;
    }
    edit_rotation_group_finalize(C, *tree, *viewer, *session);
  }
  if (!topology_move && session->recipe == EditRecipe::Offset &&
      session->transform_kind == GeometryViewerTransform::Move)
  {
    if (!session->reusing_move) {
      GeometryViewerMoveBatch &batch = session->move_batch.emplace();
      batch.capture_node_identifier = session->capture_node_identifier;
      batch.operation_node_identifier = session->operation_node_identifier;
      batch.domain = session->edit_domain;
      batch.capture_signature = session->source_signature;
      batch.indices = session->capture_indices;
      batch.point_indices = session->capture_point_indices;
      batch.links = session->recipe_links;
    }
    session->move_batch->offset = session->offset;
  }
  else {
    session->move_batch.reset();
  }
  session->reusing_move = false;
  session->native_in_callback = false;
  session->native_operator = nullptr;
  GeometryViewerEditResume resume{*session,
                                  session->duplicate_join_node_identifier ?
                                      session->duplicate_join_node_identifier :
                                      session->operation_node_identifier,
                                  edit_result_positions(*session)};
  edit_transaction_commit(C, *owner, *tree, *viewer, *session, std::move(resume));
  return true;
}

static void session_end_by_uid(bContext &C, const uint32_t uid)
{
  Main &bmain = *CTX_data_main(&C);
  if (auto *session = selection_sessions().lookup_ptr(uid)) {
    if (session->native_operator) {
      session->native_invalidated = session->native_end_requested = true;
      if (!session->native_in_callback) {
        WM_event_modal_handler_cancel(&C, session->native_operator);
      }
      /* Cancellation releases TransInfo before its finish callback deletes the proxy. If the
       * operator is initializing/executing, its own exit seam performs the deferred cleanup. */
      return;
    }
  }
  auto session = selection_session_take(uid);
  if (!session) {
    return;
  }
  edit_transaction_rollback(C, *session);
  SpaceNode *owner = find_owner_space(bmain, *session);
  if (bNodeTree *tree = find_tree(bmain, session->tree_session_uid)) {
    if (bNode *viewer = tree->node_by_id(session->viewer_identifier)) {
      if (viewer->type_legacy == GEO_NODE_VIEWER && viewer->storage) {
        auto &storage = *static_cast<NodeGeometryViewer *>(viewer->storage);
        storage.selection_enabled = storage.editing_enabled = false;
        storage.selection_object_session_uid = 0;
      }
    }
  }
  Scene *scene = id_cast<Scene *>(
      BKE_libblock_find_session_uid(&bmain, ID_SCE, session->scene_session_uid));
  if (Object *proxy = find_object(bmain, uid)) {
    Scene *cleanup_scene = scene ? scene : CTX_data_scene(&C);
    if (cleanup_scene) {
      temporary_selection_object_delete(bmain, *cleanup_scene, *proxy);
    }
  }
  else if (Mesh *mesh = find_mesh(bmain, session->proxy_mesh_session_uid)) {
    retired_proxy_mesh_uids().append_non_duplicates(mesh->id.session_uid);
    BKE_id_delete(&bmain, &mesh->id);
  }
  selection_context_restore(C, owner, *session);
  WM_event_add_notifier(&C, NC_NODE | ND_DISPLAY, nullptr);
}

void geometry_viewer_selection_end_all(bContext &C)
{
  edit_resume().reset();
  Vector<uint32_t> uids;
  for (const uint32_t uid : selection_sessions().keys()) {
    uids.append(uid);
  }
  for (const uint32_t uid : uids) {
    session_end_by_uid(C, uid);
  }
}

void geometry_viewer_selection_undo_cleanup(bContext &C)
{
  edit_resume().reset();
  /* The initial home-file read runs before a Main is installed in the context. */
  Main *main = CTX_data_main(&C);
  if (!main) {
    return;
  }
  Main &bmain = *main;
  /* Deletion may add to the retired list; iterate a copy. */
  const Vector<uint32_t> retired_uids(retired_proxy_uids());
  for (const uint32_t uid : retired_uids) {
    Object *object = find_object(bmain, uid);
    if (object && (object->id.tag & ID_TAG_RUNTIME)) {
      temporary_selection_object_delete(bmain, *CTX_data_scene(&C), *object);
    }
  }
  for (const uint32_t uid : retired_proxy_mesh_uids()) {
    if (Mesh *mesh = find_mesh(bmain, uid)) {
      if (mesh->id.tag & ID_TAG_RUNTIME) {
        BKE_id_delete(&bmain, &mesh->id);
      }
    }
  }
  for (bNodeTree &tree : bmain.nodetrees) {
    for (bNode &node : tree.nodes) {
      if (node.type_legacy == GEO_NODE_VIEWER && node.storage) {
        auto &storage = *static_cast<NodeGeometryViewer *>(node.storage);
        storage.selection_enabled = storage.editing_enabled = false;
        storage.selection_object_session_uid = 0;
      }
    }
  }
}

static void edit_resume_process(bContext &C)
{
  if (!edit_resume()) {
    return;
  }
  GeometryViewerEditResume resume = std::move(*edit_resume());
  edit_resume().reset();
  const GeometryViewerSelectionSession &saved = resume.session;
  Main &bmain = *CTX_data_main(&C);
  SpaceNode *owner = find_owner_space(bmain, saved, true);
  bNodeTree *tree = find_tree(bmain, saved.tree_session_uid);
  bNode *viewer = tree ? tree->node_by_id(saved.viewer_identifier) : nullptr;
  Object *source = find_object(bmain, saved.source_object_session_uid);
  if (!selection_sessions().is_empty() || !owner || owner->edittree != tree || !viewer ||
      viewer->type_legacy != GEO_NODE_VIEWER || !source ||
      source->object_to_world() != saved.source_to_world)
  {
    return;
  }
  bNodeLink *link = viewer_geometry_link(*tree, *viewer);
  const Mesh *mesh = viewer_mesh_get(*owner, *viewer);
  if (!link || link->fromnode->identifier != resume.result_node_identifier || !mesh ||
      (resume.node_layout && link->fromsock->identifier != saved.source_socket_identifier) ||
      nodes::geometry_edit::mesh_signature(*mesh).topology_hash !=
          saved.proxy_signature.topology_hash ||
      !edit_positions_match(mesh->vert_positions(), resume.positions))
  {
    WM_global_report(RPT_WARNING, "Viewer editing context changed; enable editing again");
    return;
  }
  wmWindow *owner_window = nullptr;
  ScrArea *owner_area = nullptr;
  for (wmWindow &window : CTX_wm_manager(&C)->windows) {
    bScreen *screen = WM_window_get_active_screen(&window);
    if (!screen) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype == SPACE_NODE &&
          uintptr_t(area.spacedata.first()) == saved.owner_space_token)
      {
        owner_window = &window;
        owner_area = &area;
      }
    }
  }
  if (!owner_window || !owner_area) {
    return;
  }
  wmWindow *previous_window = CTX_wm_window(&C);
  ScrArea *previous_area = CTX_wm_area(&C);
  ARegion *previous_region = CTX_wm_region(&C);
  CTX_wm_window_set(&C, owner_window);
  CTX_wm_area_set(&C, owner_area);
  CTX_wm_region_set(&C, nullptr);
  Scene *scene = CTX_data_scene(&C);
  wmOperator enable_op{};
  enable_op.reports = CTX_wm_reports(&C);
  static_cast<NodeGeometryViewer *>(viewer->storage)->selection_domain = int8_t(saved.edit_domain);
  if (scene && scene->id.session_uid == saved.scene_session_uid && scene->r.cfra == saved.frame &&
      scene->r.subframe == saved.subframe && CTX_data_mode_enum(&C) == CTX_MODE_OBJECT &&
      selection_enable(C, enable_op, *viewer, true) == OPERATOR_FINISHED)
  {
    auto &storage = *static_cast<NodeGeometryViewer *>(viewer->storage);
    Object *proxy = find_object(bmain, storage.selection_object_session_uid);
    BMEditMesh *em = BKE_editmesh_from_object(proxy);
    BM_mesh_elem_table_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);
    for (const int index : saved.picked_indices) {
      switch (saved.edit_domain) {
        case AttrDomain::Point:
          BM_vert_select_set(em->bm, BM_vert_at_index(em->bm, index), true);
          break;
        case AttrDomain::Edge:
          BM_edge_select_set(em->bm, BM_edge_at_index(em->bm, index), true);
          break;
        case AttrDomain::Face:
          BM_face_select_set(em->bm, BM_face_at_index(em->bm, index), true);
          break;
        default:
          BLI_assert_unreachable();
      }
    }
    EDBM_selectmode_flush(em->bm, em->selectmode);
    for (const int index : saved.hidden_verts) {
      BM_elem_flag_enable(BM_vert_at_index(em->bm, index), BM_ELEM_HIDDEN);
    }
    for (const int index : saved.hidden_edges) {
      BM_elem_flag_enable(BM_edge_at_index(em->bm, index), BM_ELEM_HIDDEN);
    }
    for (const int index : saved.hidden_faces) {
      BM_elem_flag_enable(BM_face_at_index(em->bm, index), BM_ELEM_HIDDEN);
    }
    if (saved.active_element_index >= 0) {
      const int index = saved.active_element_index;
      switch (saved.active_element_type) {
        case BM_VERT:
          BM_select_history_store(em->bm, BM_vert_at_index(em->bm, index));
          break;
        case BM_EDGE:
          BM_select_history_store(em->bm, BM_edge_at_index(em->bm, index));
          break;
        case BM_FACE:
          BM_select_history_store(em->bm, BM_face_at_index(em->bm, index));
          BM_mesh_active_face_set(em->bm, BM_face_at_index(em->bm, index));
          break;
      }
    }
    if (auto *session = selection_sessions().lookup_ptr(storage.selection_object_session_uid)) {
      session->move_batch = saved.move_batch;
      if (session->move_batch &&
          !edit_move_batch_nodes_valid(*tree, *session, session->move_batch->offset))
      {
        session->move_batch.reset();
      }
    }
    WM_event_add_notifier(&C, NC_GEOM | ND_SELECT, proxy->data);
  }
  CTX_wm_window_set(&C, previous_window);
  CTX_wm_area_set(&C, previous_area);
  CTX_wm_region_set(&C, previous_region);
}

void geometry_viewer_selection_update(bContext &C)
{
  edit_resume_process(C);
  if (selection_sessions().is_empty()) {
    return;
  }
  Main &bmain = *CTX_data_main(&C);
  Vector<uint32_t> uids;
  for (const uint32_t uid : selection_sessions().keys()) {
    uids.append(uid);
  }
  for (const uint32_t uid : uids) {
    auto *session = selection_sessions().lookup_ptr(uid);
    if (!session || session->extrusion_preparing || session->loop_cut_preparing ||
        session->deletion_preparing || session->merge_preparing ||
        session->duplication_preparing || session->face_tool_preparing ||
        session->rotation_group_preparing)
    {
      continue;
    }
    SpaceNode *owner = find_owner_space(bmain, *session, true);
    bNodeTree *tree = find_tree(bmain, session->tree_session_uid);
    bNode *viewer = tree ? tree->node_by_id(session->viewer_identifier) : nullptr;
    Object *proxy = find_object(bmain, uid);
    const bool basic_valid = owner && owner->edittree == tree && viewer &&
                             viewer->type_legacy == GEO_NODE_VIEWER &&
                             find_object(bmain, session->source_object_session_uid) && proxy &&
                             BKE_editmesh_from_object(proxy);
    if (!basic_valid ||
        (session->editing && !edit_session_validate(C, *owner, *viewer, *session, true)))
    {
      session_end_by_uid(C, uid);
      WM_global_report(RPT_WARNING,
                       "Viewer session ended because its mesh or editing context changed");
    }
    else if (session->move_batch && !session->native_operator) {
      const BMEditMesh *em = BKE_editmesh_from_object(proxy);
      if (!edit_move_batch_selection_matches(*em, *session->move_batch) ||
          !edit_move_batch_nodes_valid(*tree, *session, session->move_batch->offset))
      {
        session->move_batch.reset();
      }
    }
  }
}

int geometry_viewer_selection_count(const Main &bmain,
                                    const bNode &viewer_node,
                                    int &r_selection_domain)
{
  if (!viewer_node.storage) {
    return -1;
  }
  const NodeGeometryViewer &storage = *static_cast<const NodeGeometryViewer *>(
      viewer_node.storage);
  if (!storage.selection_enabled || storage.selection_object_session_uid == 0) {
    return -1;
  }

  Object *object = find_object(const_cast<Main &>(bmain), storage.selection_object_session_uid);
  BMEditMesh *edit_mesh = object ? BKE_editmesh_from_object(object) : nullptr;
  if (!edit_mesh) {
    return -1;
  }

  const std::optional<AttrDomain> domain = selection_domain_from_mode(edit_mesh->selectmode);
  if (!domain) {
    return -1;
  }
  r_selection_domain = int(*domain);
  switch (*domain) {
    case AttrDomain::Point:
      return edit_mesh->bm->totvertsel;
    case AttrDomain::Edge:
      return edit_mesh->bm->totedgesel;
    case AttrDomain::Face:
      return edit_mesh->bm->totfacesel;
    default:
      return -1;
  }
}

void NODE_OT_geometry_viewer_selection_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Viewer Selection";
  ot->idname = "NODE_OT_geometry_viewer_selection_toggle";
  ot->description = "Select elements of the Viewer mesh in the 3D Viewport";
  ot->poll = geometry_viewer_selection_poll;
  ot->exec = geometry_viewer_selection_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;
  node_identifier_property_define(*ot);
}

void NODE_OT_geometry_viewer_import_selection(wmOperatorType *ot)
{
  ot->name = "Import Viewer Selection";
  ot->idname = "NODE_OT_geometry_viewer_import_selection";
  ot->description = "Create a Select Elements node from the selected Viewer mesh elements";
  ot->poll = geometry_viewer_selection_poll;
  ot->exec = geometry_viewer_import_selection_exec;
  ot->flag = OPTYPE_UNDO;
  node_identifier_property_define(*ot);
}

void NODE_OT_geometry_viewer_editing_toggle(wmOperatorType *ot)
{
  ot->name = "Viewer Edit Mode";
  ot->idname = "NODE_OT_geometry_viewer_editing_toggle";
  ot->description =
      "Begin a stage-locked mesh authoring session; disable to discard pending edits";
  ot->poll = geometry_viewer_selection_poll;
  ot->exec = geometry_viewer_editing_toggle_exec;
  node_identifier_property_define(*ot);
}

void NODE_OT_geometry_viewer_edit_transaction(wmOperatorType *ot)
{
  ot->name = "Viewer Edit Transaction";
  ot->idname = "NODE_OT_geometry_viewer_edit_transaction";
  ot->description = "Validate a Viewer editing session or abort a pending topology operation";
  ot->poll = geometry_viewer_selection_poll;
  ot->exec = geometry_viewer_edit_transaction_exec;
  ot->flag = OPTYPE_INTERNAL;
  node_identifier_property_define(*ot);
  static const EnumPropertyItem actions[] = {
      {EDIT_VALIDATE,
       "VALIDATE",
       0,
       "Validate",
       "End sessions whose context or input is no longer valid"},
      {EDIT_ABORT_EXTRUSION,
       "ABORT_EXTRUSION",
       0,
       "Abort Entire Extrusion",
       "Remove the entire pending extrusion and exit Viewer editing"},
      {EDIT_ABORT_LOOP_CUT,
       "ABORT_LOOP_CUT",
       0,
       "Abort Entire Loop Cut",
       "Remove the entire pending loop cut and exit Viewer editing"},
      {EDIT_ABORT_DUPLICATION,
       "ABORT_DUPLICATION",
       0,
       "Abort Entire Duplication",
       "Remove the entire pending duplication and exit Viewer editing"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  PropertyRNA *action = RNA_def_enum(
      ot->srna, "action", actions, EDIT_VALIDATE, "Action", "Transaction stage");
  RNA_def_property_flag(action, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void NODE_OT_geometry_viewer_edit_transform(wmOperatorType *ot)
{
  ot->name = "Viewer Transform";
  ot->idname = "NODE_OT_geometry_viewer_edit_transform";
  ot->description = "A viewport transform recorded as editable Geometry Nodes";
  /* Replace the native operator's history entry after commit. A native redo/repeat cannot act on
   * a retired proxy: users edit the generated node controls, or use ordinary graph undo/redo. */
  ot->poll = [](bContext *C) {
    CTX_wm_operator_poll_msg_set(C,
                                 "Adjust the generated nodes, or start another Viewer transform");
    return false;
  };
  ot->exec = [](bContext *, wmOperator *) { return OPERATOR_CANCELLED; };
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;
}

void NODE_OT_geometry_viewer_edit_extrude(wmOperatorType *ot)
{
  ot->name = "Viewer Extrude";
  ot->idname = "NODE_OT_geometry_viewer_edit_extrude";
  ot->description =
      "Create an Extrude Mesh node from the selected Viewer elements and move its Top";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->exec = [](bContext *C, wmOperator *op) {
    return geometry_viewer_edit_extrude_start(C, op, nullptr);
  };
  ot->invoke = geometry_viewer_edit_extrude_start;
  /* Like the native E wrapper, only the child transform creates undo/history. */
  ot->flag = 0;
  RNA_def_boolean(ot->srna,
                  "individual",
                  false,
                  "Individual Faces",
                  "Extrude separate faces with a shared captured movement instead of one region");
  const float offset[3] = {};
  RNA_def_float_vector(ot->srna,
                       "offset",
                       3,
                       offset,
                       -FLT_MAX,
                       FLT_MAX,
                       "Offset",
                       "World-space extrusion movement for non-interactive execution",
                       -100,
                       100);
}

void NODE_OT_geometry_viewer_edit_face(wmOperatorType *ot)
{
  static const EnumPropertyItem types[] = {
      {VIEWER_FACE_TRIANGULATE, "TRIANGULATE", 0, "Triangulate", "Triangulate selected faces"},
      {VIEWER_FACE_FLIP, "FLIP", 0, "Flip Faces", "Reverse the winding of selected faces"},
      {VIEWER_FACE_SMOOTH, "SMOOTH", 0, "Shade Smooth", "Set selected faces to smooth shading"},
      {VIEWER_FACE_FLAT, "FLAT", 0, "Shade Flat", "Set selected faces to flat shading"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Viewer Face Tools";
  ot->idname = "NODE_OT_geometry_viewer_edit_face";
  ot->description = "Record selected face edits as ordinary, editable Geometry Nodes";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->invoke = WM_menu_invoke;
  ot->exec = geometry_viewer_edit_face_exec;
  /* The menu creates no transaction; a selected action creates one graph undo step. */
  ot->flag = OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          types,
                          VIEWER_FACE_TRIANGULATE,
                          "Type",
                          "Face operation to capture in the node graph");
  RNA_def_enum(ot->srna,
               "quad_method",
               rna_enum_modifier_triangulate_quad_method_items,
               int(geometry::TriangulateQuadMode::Beauty),
               "Quad Method",
               "Method for splitting quads; editable on the generated Triangulate node");
  RNA_def_enum(ot->srna,
               "ngon_method",
               rna_enum_modifier_triangulate_ngon_method_items,
               int(geometry::TriangulateNGonMode::Beauty),
               "N-gon Method",
               "Method for splitting n-gons; editable on the generated Triangulate node");
}

void NODE_OT_geometry_viewer_edit_merge(wmOperatorType *ot)
{
  static const EnumPropertyItem modes[] = {
      {GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL, "ALL", 0, "All", "Merge nearby selected vertices"},
      {GEO_NODE_MERGE_BY_DISTANCE_MODE_CONNECTED,
       "CONNECTED",
       0,
       "Connected",
       "Merge selected vertices along existing edges"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Viewer Merge by Distance";
  ot->idname = "NODE_OT_geometry_viewer_edit_merge";
  ot->description =
      "Capture selected Viewer vertices and insert an editable Merge by Distance node";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->exec = geometry_viewer_edit_merge_exec;
  ot->invoke = [](bContext *C, wmOperator *op, const wmEvent *) {
    return WM_operator_props_dialog_popup(C, op, 300, std::nullopt, IFACE_("Merge"));
  };
  ot->flag = OPTYPE_UNDO;
  RNA_def_enum(ot->srna,
               "mode",
               modes,
               GEO_NODE_MERGE_BY_DISTANCE_MODE_ALL,
               "Mode",
               "The existing Merge by Distance node mode; center, cursor and active merging are "
               "separate operations");
  PropertyRNA *distance = RNA_def_float_distance(ot->srna,
                                                 "distance",
                                                 0.001f,
                                                 0,
                                                 FLT_MAX,
                                                 "Distance",
                                                 "Merge distance in mesh-local space",
                                                 0,
                                                 1);
  RNA_def_property_subtype(distance, PROP_DISTANCE);
}

void NODE_OT_geometry_viewer_edit_duplicate(wmOperatorType *ot)
{
  ot->name = "Viewer Duplicate and Move";
  ot->idname = "NODE_OT_geometry_viewer_edit_duplicate";
  ot->description =
      "Duplicate the connected Viewer selection, preserve the source, and move the copy";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->exec = [](bContext *C, wmOperator *op) {
    return geometry_viewer_edit_duplicate_start(C, op, nullptr);
  };
  ot->invoke = geometry_viewer_edit_duplicate_start;
  /* The child transform owns its one undo/history entry, including zero-offset cancellation. */
  ot->flag = 0;
  const float offset[3] = {};
  RNA_def_float_vector(ot->srna,
                       "offset",
                       3,
                       offset,
                       -FLT_MAX,
                       FLT_MAX,
                       "Offset",
                       "World-space movement of the copy for non-interactive execution",
                       -100,
                       100);
}

void NODE_OT_geometry_viewer_edit_loop_cut(wmOperatorType *ot)
{
  ot->name = "Viewer Loop Cut and Slide";
  ot->idname = "NODE_OT_geometry_viewer_edit_loop_cut";
  ot->description = "Insert editable loop cuts on the hovered quad ring, then slide them";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->exec = [](bContext *C, wmOperator *op) {
    return geometry_viewer_edit_loop_cut_start(C, op, nullptr);
  };
  ot->invoke = geometry_viewer_edit_loop_cut_start;
  ot->modal = geometry_viewer_edit_loop_cut_modal;
  ot->cancel = [](bContext *C, wmOperator *op) {
    if (op->customdata) {
      loop_cut_finish(*C, *op, false, true);
    }
  };
  /* No native repeat/Adjust Last Operation: the input is captured at this geometry stage. */
  ot->flag = OPTYPE_UNDO;
  PropertyRNA *edge = RNA_def_int(
      ot->srna,
      "edge_index",
      -1,
      -1,
      INT_MAX,
      "Seed Edge",
      "Seed crossing edge at the current Viewer stage (script execution)",
      -1,
      INT_MAX);
  RNA_def_property_flag(edge, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_int(ot->srna, "cuts", 1, 1, 100, "Cuts", "Number of loops to insert", 1, 100);
  RNA_def_float(ot->srna,
                "factor",
                0,
                -1,
                1,
                "Factor",
                "Signed slide in the node's stored seed-edge direction",
                -1,
                1);
}

void NODE_OT_geometry_viewer_edit_delete(wmOperatorType *ot)
{
  static const EnumPropertyItem types[] = {
      {VIEWER_DELETE_VERTICES,
       "VERT",
       0,
       "Vertices",
       "Delete selected vertices and their incident edges and faces"},
      {VIEWER_DELETE_EDGES,
       "EDGE",
       0,
       "Edges",
       "Delete selected edges and their incident faces using Delete Geometry's All mode"},
      {VIEWER_DELETE_FACES,
       "FACE",
       0,
       "Faces",
       "Delete selected faces using Delete Geometry's All mode"},
      {VIEWER_DELETE_EDGE_FACE,
       "EDGE_FACE",
       0,
       "Only Edges & Faces",
       "Delete selected edges and incident faces, retaining vertices"},
      {VIEWER_DELETE_ONLY_FACE,
       "ONLY_FACE",
       0,
       "Only Faces",
       "Delete selected faces, retaining edges and vertices"},
      {0, "", 0, nullptr, nullptr},
      {VIEWER_DISSOLVE_VERTICES,
       "DISSOLVE_VERT",
       0,
       "Dissolve Vertices",
       "Remove selected vertices and merge surrounding faces using Dissolve Elements"},
      {VIEWER_DISSOLVE_EDGES,
       "DISSOLVE_EDGE",
       0,
       "Dissolve Edges",
       "Merge faces across selected eligible edges using Dissolve Elements"},
      {VIEWER_DISSOLVE_FACES,
       "DISSOLVE_FACE",
       0,
       "Dissolve Faces",
       "Merge connected selected faces using Dissolve Elements"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Viewer Delete / Dissolve";
  ot->idname = "NODE_OT_geometry_viewer_edit_delete";
  ot->description =
      "Create a Delete Geometry or Dissolve Elements node from the selected Viewer elements";
  ot->poll = geometry_viewer_edit_tool_poll;
  ot->invoke = WM_menu_invoke;
  ot->exec = geometry_viewer_edit_delete_exec;
  /* A menu cancellation never authors nodes. A chosen action creates one global graph undo step,
   * without native repeat/Adjust Last Operation targeting a retired runtime proxy. */
  ot->flag = OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          types,
                          VIEWER_DELETE_VERTICES,
                          "Type",
                          "Operation to capture in the node graph");
}

}  // namespace blender::ed::space_node
