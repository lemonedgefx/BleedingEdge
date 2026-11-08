/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix_types.hh"

namespace blender {
struct bContext;
struct bNode;
struct Main;
struct wmOperator;
struct wmOperatorType;

namespace ed::space_node {

/** Returns -1 when the runtime selection proxy is no longer available. */
int geometry_viewer_selection_count(const Main &bmain,
                                    const bNode &viewer,
                                    int &r_selection_domain);

/** Authoring-session seams shared by the Viewer UI, event loop, undo and file lifecycle. */
bool geometry_viewer_edit_operator_poll(bContext &C, const wmOperatorType &operator_type);
bool geometry_viewer_selection_owns_edit_mesh(const bContext &C);
/** Node selection in the owning editor must neither snapshot the proxy nor end the session. */
bool geometry_viewer_selection_skip_operator_undo(const bContext &C,
                                                  const wmOperatorType &operator_type);
/** Retire the proxy for a real layout undo snapshot, then restore picking on the next update. */
bool geometry_viewer_node_layout_undo_push_prepare(bContext &C,
                                                   const wmOperatorType &operator_type);
/** Preserve picking across a layout-only undo/redo; never retain pointers into undo Main data. */
void geometry_viewer_node_layout_undo_begin(bContext &C, bool layout_step);
void geometry_viewer_node_layout_undo_end(bContext &C);
bool geometry_viewer_editing_owns_edit_mesh(const bContext &C);
bool geometry_viewer_selection_is_active();
bool geometry_viewer_edit_has_pending(const bNode &viewer);
bool geometry_viewer_edit_transform_is_running(const bNode &viewer);
bool geometry_viewer_edit_extrusion_is_pending(const bNode &viewer);
bool geometry_viewer_edit_duplication_is_pending(const bNode &viewer);

enum class GeometryViewerLoopCutStage { None, Cuts, Slide };
GeometryViewerLoopCutStage geometry_viewer_edit_loop_cut_stage(const bNode &viewer);

enum class GeometryViewerTransform { Move, Scale, Rotate };

/** Native transforms own the proxy from begin until finish, after TransInfo has been freed.
 * Update only authors nodes; it must never replace or write the native transform's BMesh. */
bool geometry_viewer_edit_transform_begin(bContext &C,
                                          wmOperator &op,
                                          GeometryViewerTransform kind,
                                          bool &r_topology_move);
bool geometry_viewer_edit_transform_update(bContext &C,
                                           GeometryViewerTransform kind,
                                           const float4x4 &world_transform,
                                           const float3 &world_pivot,
                                           int pivot_mode,
                                           bool pivot_override);
/** Returns true only for a non-empty, committed graph edit. */
bool geometry_viewer_edit_transform_finish(bContext &C,
                                           wmOperator &op,
                                           bool confirmed,
                                           bool abort_operation = false);
void geometry_viewer_selection_update(bContext &C);
void geometry_viewer_selection_end_all(bContext &C);
void geometry_viewer_selection_undo_cleanup(bContext &C);

}  // namespace ed::space_node
}  // namespace blender
