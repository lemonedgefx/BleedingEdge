# Geometry Nodes interactive editing: research and roadmap

Research date: 2026-10-11. Source snapshot: local `bleeding-edge` branch at
`972e506154c`, with pre-existing uncommitted work preserved. This document records
source findings and proposed constraints, plus the Phase 0–4 implementation notes
below. Source links below are relative to this document; line anchors refer
to the inspected snapshot and may move as the branch changes.

The next-phase changes authorized on 2026-10-12 are recorded below. Those changes
supersede the historical separate Captured Selection and Preview Offset workflows
in the earlier phase notes; the earlier verification results remain historical.

## Build verification update (2026-10-18)

The single BEX-disabled Windows Release build now includes the previously source-only accumulated
Move, loop-cut hover preservation, Set Rotation grouping, and Select Elements rename. This update
supersedes their original build-pending notes below; it does not authorize deferred features.

- `geometry_viewer_editing_smoke.py`: unified manual/captured selection, native movement/undo,
  identity/deformation guards and large-capture persistence passed.
- `geometry_viewer_move_accumulation_test.py`: nine native adapter scenarios passed.
- `geometry_viewer_move_accumulation_ui_smoke.py`: real G/shared-gizmo-path gestures,
  borrowed-node cancellation/no-op, mode changes, undo/redo and session boundaries passed.
- `geometry_viewer_loop_cut_hover_ui_smoke.py`: real Ctrl+R off-mesh scrolling/clicking,
  resumed hover, cancellation and frame-abort safeguards passed.
- `geometry_viewer_rotation_group_test.py -- --adapter`: 121 automatic C++ grouping checks,
  including edited rotation controls, shear/basis handling, replay, copying and persistence passed.
- Loop-cut and extrusion adapter suites, 25 face-tool checks, and 21 merge/duplicate checks passed.
- Native instantiation confirms the public Select Elements and Loop Selection labels.

The deferred curve roadmap, bevel, and automatic normal-extrusion Offset Scale/vector switching
remain outside this implementation. The WITH_GTESTS-disabled profile does not execute C++ GTests.
Build/test evidence is in `validation_artist_optical_flow/` under the configured build directory.

## Proposal: a procedural mesh-editing mode

**Recommendation:** extend the Viewer selection infrastructure into a native
Geometry Nodes authoring mode. The viewport provides familiar mesh selection,
widgets, shortcuts and modal input; the durable result is an editable node chain,
not an independently edited copy of the evaluated mesh. BMesh can be used for
the interaction proxy and bounded geometry algorithms. Evaluation must remain
independent of viewport state and context-bound editor operators.

The first usable release should cover the four requested interactions: move,
edge/face scale, extrusion, and loop cut/slide. Rotation belongs alongside the
transform work because it exercises the same selection, pivot and coordinate
contracts. Other tools follow after this foundation passes its verification
gates. Phase 0 has now been authorized, implemented and validated in the working tree.
Phases 1–3 and the Phase 4 **Delete/Dissolve subset** are now compiled in one
BEX-disabled Windows build. Current runtime verification is recorded under the
Phase 4 section below. The dated Phase 1–3 source-only notes are historical and
their build status is superseded by that verification. Bevel is explicitly deferred.
The Phase 4 **Merge / Duplicate-and-Move** and **selected face tools** subsets
are also compiled and verified in that same BEX-disabled profile. The earlier
source-only Merge/Duplicate checkpoint is retained below as a historical record
and is superseded by the selected-face-tools verification. The remaining toolkit
items and Phases 5–7 are **proposed**, not completed work. No BEX dependency is
introduced.

The [curve editing roadmap](geometry-nodes-curve-editing-roadmap.md), recorded
2026-10-13, is explicitly deferred and kept separate from this mesh toolkit.
Saving that proposal does not authorize its implementation or a build.

## Source update: accumulated Move and stable loop-cut hover (2026-10-15)

**Historical checkpoint:** implemented in source only on 2026-10-15. The 2026-10-18
build verification above now supersedes this checkpoint's build/runtime boundary.

- Consecutive native G or move-widget translations with the same selection/domain
  reuse the current session's last generated Select Elements / Set Position pair.
  Each displacement is converted into mesh-local space and added to the committed
  Offset, rather than appending another operation or adding the modal delta twice.
- Only a runtime record established by a confirmed translation authorizes reuse.
  Existing arbitrary/saved Set Position nodes are not adopted. Selection/domain
  changes, another modelling operation, altered controls/captures/links, or shared
  consumers end the run. Manual node layout does not change the move recipe.
  Explicitly exiting and re-entering Edit Mode starts a new pair. The internal
  proxy retirement/resume needed for safe global undo carries the runtime record;
  it is not a user exit and must not break accumulation.
- Each confirmed gesture still creates one undo step. A reused pending node is
  borrowed, not owned: Escape/no-op restores its previous Offset without deleting
  the committed pair. Forced abort likewise restores only the value written by
  that gesture, without overwriting an external value/link change. A native G to
  S/R mode switch restores the earlier move, then authors a separate recipe on
  that result. Native BMesh ownership and final mesh replay checks remain intact.
- Ctrl+R placement now returns normally on a missing or invalid hovered ring,
  before replacing a durable capture with empty indices. Overlay/placement state
  is separate from the last valid candidate. Empty space hides the ring overlay,
  preserves the chosen count and stays in Edit Mode; invalid edges keep their
  diagnostic overlay. Returning to a valid edge applies the current count, while
  clicks without a valid ring do not commit. Genuine frame/input/context changes
  and explicit abort retain the existing shutdown safeguards.
- Regression tests were added before the source changes. The existing executable
  fails `geometry_viewer_move_accumulation_test.py` at the second move and
  `geometry_viewer_loop_cut_hover_ui_smoke.py` after moving off the mesh, matching
  the reported symptoms. The ordinary-node recipe test passes **30** cumulative
  offset/restoration checks across Point/Edge/Face, nonuniform scale and inherited
  shear. This proves the field recipe, not the new C++ runtime adapter.
- `geometry_viewer_move_accumulation_ui_smoke.py` prepares real G, the shared
  gizmo translate invocation seam, borrowed-node cancellation/no-op, G-to-R,
  per-gesture undo/redo, explicit re-entry and frame-abort checks. The hover test
  covers off-mesh scrolling/clicking, return hover, cancel and real frame changes.
  New-adapter execution and C++ compilation were pending at this checkpoint; both are now covered
  by the 2026-10-18 build verification above.
- Source-only checks pass: formatting, whitespace, Python syntax for all **34**
  Viewer scripts, and documentation TypeScript checking without emitting a build.
  The existing executable also passes the **39** transform-recipe and **1013**
  loop-cut-recipe checks. These are ordinary-node/kernel checks, not verification
  of the newly changed C++ adapter.

The proposed automatic normal-extrusion Offset Scale / explicit-vector switching
is a separate, unimplemented change; it is not included in this request.

## Source update: Set Rotation group and Select Elements (2026-10-13)

**Historical checkpoint:** this was source-only on 2026-10-13. Automatic grouping is now built
and adapter-tested as recorded in the 2026-10-18 update above. This update changes
new confirmed rotations; it does not retroactively rewrite saved rotation chains
or implement the deferred curve roadmap.

- Select by Index is renamed **Select Elements** in the visible UI and import
  description. The `GeometryNodeSelectByIndex` type identifier, storage, socket
  identifiers, Manual/Captured modes and index/range behavior are unchanged, so
  existing projects and type-identifier-based scripts retain compatibility.
  Scripts that hard-code a newly generated default node name may need updating.
  Historical notes below
  keep the name used at their recorded checkpoint.
- A new confirmed rotation becomes one **Set Rotation** Geometry Nodes group,
  containing the protected Select Elements capture and the operation through
  Set Position. Its public interface is **Mesh input, Rotation input, Mesh
  output**. Rotation uses the built-in Rotation socket's readable XYZ angle
  controls. Each gesture gets an independent group/capture; Blender may suffix
  the datablock name for uniqueness, while its visible label stays Set Rotation.
- The capture's original domain, affected-point arrays, identity metadata and
  fixed/median/active pivot data are deep-copied into the group. Group geometry
  input feeds both capture and Set Position; Points feeds Set Position Selection.
  Position minus Pivot is transformed, then Pivot is added back.
- Combine Transform creates the editable world rotation R. A captured affine
  object basis A and ordinary Invert Matrix/Multiply Matrices produce
  **A^-1 R A**. This preserves rotation under nonuniform unapplied scale and
  inherited shear; decomposing that local matrix to rotation/scale would lose
  the original viewport correspondence. The captured basis is fixed, matching
  the existing snapshot semantics rather than introducing a live self-object
  dependency.
- The existing raw recipe stays pending during the gesture. After TransInfo is
  freed, confirmation builds a candidate group beside that recipe, evaluates it,
  compares the full mesh result with the native transform and validates context,
  links, controls and propagated nested capture warnings. Only then are the old
  root helpers/capture/Set Position replaced by the group.
- The group inherits frame parenting and occupies the old capture's position.
  Viewer compacts to the next width-spaced position unless manually moved.
  Source mesh, Group Output and sibling branches remain unchanged. Resume uses
  the group result and retains picking, active history and hiding.
- Cancel/no-op creates no durable group. Failed/invalidated preparation rolls
  back owned nodes and discards an unreferenced candidate group ID. Commit keeps
  the group and retires the proxy before the existing single graph undo step.
  Move, Scale, Extrude, Loop Cut and other operation recipes are not grouped by
  this change. New groups are ordinary editable node groups, not new opaque
  geometry node types.

### Verification boundary

- [Recipe and adapter contract](../tests/python/geometry_viewer_rotation_group_test.py)
  default/prototype mode passes **121 checks** on the existing executable. It
  reconstructs the ordinary-node group around actual captured selections and
  checks Point/Edge/Face picks, edited XYZ values, nonuniform scale and parent
  shear, median/active/cursor/explicit pivots, position-only replay, same-count
  ID reordering guards, group copying, attributes and save/reload.
- That prototype mode **does not validate the new C++ grouping adapter or
  visible rename**. Its `-- --adapter` mode and the new
  [real-input GUI test](../tests/python/geometry_viewer_rotation_group_ui_smoke.py)
  are prepared for a later authorized build. GUI coverage includes R/Z/45,
  compact interface, continuation, angle editing, cancellation/no-op, one-step
  undo/redo and save cleanup.
- Existing transform, pivot-replay and layout scripts are updated to find the
  protected capture inside the group and expect one root rotation node. Pending
  gesture/cancellation tests continue to expect the original ungrouped safety
  recipe. These changed adapter tests have not run on new compiled source.
  Mixed selection-replay tests also resolve geometry outputs by type, so Mesh
  output on Set Rotation does not break an extrude/scale/move/rotate chain.
- Keymap checks pass (**144**), all **30** Viewer Python scripts parse, scoped
  C++ formatting/whitespace and wiki TypeScript no-emit checks pass. No C++
  compile, GTest, new GUI-adapter run, build, commit or publication is claimed.

## Viewer usability fixes (2026-10-12)

Two regressions were reproduced on the previous build: two confirmed moves put
their operation nodes at the same coordinates, and a real node-selection mouse
click ended Edit Mode. The source now uses one width-aware horizontal layout for
all edit recipes, advancing Viewer after the new nodes. Frame children use
absolute coordinates. Recipe changes retain their original insertion position;
cancel restores Viewer unless the user moved it manually. Other branches and
existing nodes are not rearranged, including overlaps saved by the old build.

The node-click exit came from the non-viewport undo safeguard. Ordinary selection
operators in the owning node editor now skip temporary-proxy undo snapshots,
without ending the session. Real property edits retain ordinary global undo and
retire the proxy first. Select Link Viewer is deliberately not exempted because
it can create and rewire nodes.

Captured Selection was a design choice for a locked, serialized stage snapshot.
Select by Index already adapts edge/face masks to vertices, but it does not check
the geometry that the original selection came from. Structured captured arrays
also avoid the size limit of an editable index-string default. Combining both
behaviours in an extended Select by Index would be possible; that node redesign
was not part of these fixes. The user subsequently requested and authorized that
consolidation as the next-phase task below.

Regression tests:
[generated layout](../tests/python/geometry_viewer_edit_layout_smoke.py) and
[real node-selection clicks](../tests/python/geometry_viewer_node_selection_ui_smoke.py).
Both failed on the previous executable and pass on the updated one. The layout
test passes 24 checks covering consecutive edits, transform helpers, cut,
dissolve, extrude, delete, frame parenting, cancel and manual Viewer movement.
The click test uses actual mouse/key events, covers pending previews, and confirms
that unrelated property edits still close the session and undo correctly.

One Windows x64 Release `INSTALL` build completed successfully with `WITH_BEX=OFF`:
`C:\Users\rajee\Development\build_windows_x64_vc17_Release_no_bex\bin\Release\blender.exe`
(Blender 5.3.0 Alpha, local working tree). No BEX-enabled build was produced.
All processes below exited cleanly on this executable:

- `GEOMETRY_VIEWER_EDIT_LAYOUT_SMOKE_OK 24`
- `GEOMETRY_VIEWER_NODE_SELECTION_UI_SMOKE_OK`
- Existing Selection, Editing, Transform, Extrude, Loop Cut and Delete adapter
  smoke tests; Transform reports 9 cases and Delete reports 25 checks.
- Existing Editing, Transform, Extrude, Loop Cut and Delete UI tests.
- Transform/Extrude/Loop Cut recipe contracts: 39 / 50 / 1013 checks.
- `BEX_DISABLED_FINAL_BUILD_OK`: BEX is disabled, scripting nodes are absent,
  and the custom selection and modelling nodes remain registered.

The keymap routing test passes 64 checks; all 17 Viewer Python test files parse;
the wiki TypeScript check and changed C++ formatting/whitespace checks pass.
GTests remain disabled in this profile, so no GTest execution is claimed.

## Next-phase task: node movement during Viewer Edit Mode (2026-10-12)

**Status: implemented and verified on 2026-10-12 in one BEX-disabled Release build.**

Reproduced regression: the same node-header drag moves nodes with Viewer Edit
Mode off, but leaves them stationary with Edit Mode on. Node translation still
passes its operator poll; execution returns CANCELLED with the warning "Use
Move, Scale or Rotate in the 3D View for Viewer editing". Blender's shared
transform operator handles both node-layout movement and viewport mesh
transforms. The Viewer adapter detects its active temporary EditMesh regardless
of the current editor, then rejects the node-editor invocation because it is
not in the 3D View.

Authorized scope:

- Restrict Viewer mesh-transform interception to the relevant 3D View mesh
  operation; allow ordinary node dragging and G movement in the node editor.
- Keep Edit Mode and its element selection active during node-layout changes.
  Coordinate changes alone must not invalidate captured selections or author
  mesh-transform nodes. Retain safeguards for genuinely conflicting active
  viewport gestures or changed graph/evaluation inputs.
- Handle layout undo/redo safely without treating it as disposable EditMesh
  undo, losing the layout undo step, or leaving stale proxy references. The
  existing non-viewport undo safeguard must not unnecessarily end the session
  for supported layout changes. Real property edits, rewiring and other graph
  changes retain their existing validation and global-undo safeguards.
- Add real-event regressions for header dragging and G, confirmation and
  cancellation, node selection, frame/Viewer movement, layout undo/redo and
  subsequent viewport editing. Confirm that viewport G/S/R still generates
  the correct operation nodes and that unrelated graph edits remain guarded.
- Update Viewer documentation and the Miscellaneous wiki entry after the fix
  is implemented and verified.

Diagnostic evidence is retained outside the source tree under the BEX-disabled
build's `viewer_next_phase_validation` directory as `node_drag_diagnostic.py`
and its logs. Two real-event runs reproduced the blocked drag; a targeted
operator invocation confirmed the rejection warning and CANCELLED result.
The initial queue request changed only this task record. The user subsequently
authorized implementation and one BEX-disabled build.

Implementation record:

- The shared transform operator no longer routes node-editor transforms through
  the temporary Viewer EditMesh adapter. Ordinary node movement and frame
  attachment are permitted between viewport gestures; pending mesh operations
  remain guarded.
- Layout commits retire the proxy before a global undo snapshot and enqueue
  ID-based restoration of the current selection domain, picked/active elements
  and hiding. Coordinate changes create no geometry-operation nodes.
- Layout-only undo steps are named "Move Nodes (Viewer Edit Mode)". Undo/redo
  restores picking only after checking the same Viewer input, ordered geometry,
  object transform and editing context. Runtime proxy pointers are never kept
  across Main decoding; unrelated property/graph edits retain their safeguards.
- The real-event regression is
  [node layout](../tests/python/geometry_viewer_node_layout_ui_smoke.py). It failed
  on the previous executable at node-header dragging before the source fix.
  The completed regression and original diagnostic now pass on the updated build.

Verification:

- `GEOMETRY_VIEWER_NODE_LAYOUT_UI_SMOKE_OK` passes 32 timed stages with a clean
  process exit: actual node-header dragging and G, cancellation, one-step layout
  undo/redo, framed children and frame/Viewer movement, captured-node movement
  after a confirmed viewport edit, and continued viewport node authoring.
- Point, Edge, Face and empty selections survive layout commits and undo/redo.
  Checks include active-element history, active-face fallback, locally hidden
  elements, unchanged mesh positions, unchanged node counts and no leaked proxies.
- All 13 existing headless suites pass, including selection/pivot replay,
  old-file compatibility, editing, transform/extrude/loop-cut/delete adapters,
  generated layout and the three geometry recipe suites.
- All seven real-event suites pass with exit code 0: Node Layout, Node Selection,
  Editing, Transform, Extrude, Loop Cut and Delete. Existing tests retain the
  unrelated-property undo and active-gesture/context invalidation checks.
- `BEX_DISABLED_LAYOUT_BUILD_OK` confirms that the three BEX scripting node
  types are absent and the custom selection/modelling nodes remain registered.
  The existing Windows x64 Release `INSTALL` profile remains `WITH_BEX=OFF`.
- All 21 Viewer Python files parse, keymap routing passes 64 checks, wiki
  TypeScript no-emit and scoped C++ formatting/whitespace checks pass. GTests
  remain disabled in this profile; no full Blender test-suite claim is made.

Executable:
`C:\Users\rajee\Development\build_windows_x64_vc17_Release_no_bex\bin\Release\blender.exe`
(Blender 5.3.0 Alpha, build timestamp 2026-10-12 16:56:07 UTC). Build and test
logs are retained under `viewer_next_phase_validation`. No BEX-enabled build,
commit or publication was produced, and unrelated working-tree edits were
preserved. At this build checkpoint, interactive Merge and Duplicate-and-Move
were still proposed; their later source-only implementation is recorded below.
Bevel remains deferred.

## Next-phase task: deformation-safe selection replay (2026-10-12)

**Status: implemented and verified on 2026-10-12 in the BEX-disabled Release build.**

Reproduced limitation: extrude a selected face, scale the generated face, then
change the earlier extrusion's Offset. Element counts and ordered connectivity
remain unchanged, but the downstream Captured Selection becomes invalid because
its signature includes exact vertex positions. Later captures can fail in turn.
Strict snapshot validation remains in use for active gestures. Durable captured
selections now have a distinct replay contract described in the implementation
record below; the exact failure is covered by a regression test.

Authorized scope:

- Preserve captured Point/Edge/Face membership through upstream position-only
  changes when connectivity and element mapping remain valid. Counts alone are
  not sufficient validation.
- Retain safeguards for changed connectivity or element identity, including
  same-count reordering and ambiguous isolated-point/symmetric cases. Do not
  replace validation with an unconditional raw-index mask.
- Separate durable selection identity from the frozen geometry checks needed
  to protect an active viewport gesture.
- Recalculate selection-dependent scaling/rotation pivots where appropriate;
  explicitly fixed pivots must retain their intended behaviour. Removing only
  the position comparison is not a complete replay fix.
- Add an extrude -> scale -> change Offset regression and mixed-operation
  checks, including valid deformation, invalid topology/reordering, undo/redo,
  node copying and save/reload. Update the documented capture contract when
  this work is implemented and verified.

## Next-phase task: unify Select by Index and Captured Selection (2026-10-12)

**Status: implemented and verified on 2026-10-12 in the BEX-disabled Release build.**

Extend **Select by Index** to include the captured-selection capabilities, so
new manual and viewport-authored selections use one user-facing node.

Authorized scope:

- Preserve the existing comma-separated index/range input and domain behaviour
  for manually authored selections. Existing manual nodes and links must not
  acquire a mandatory Geometry input or silently change their behaviour.
- Add captured-selection support to the same node, including the source-stage
  Geometry input, captured domain/element data, affected-point selection and
  validity reporting. Preserve the capabilities of **Selection**, **Points**
  and **Valid**, with clearly defined behaviour in each usage mode.
- Retain structured storage for captures rather than relying on a long editable
  index string; large viewport selections must not be truncated by text limits.
- Make Viewer edit recipes create the extended Select by Index instead of a
  separate Captured Selection node. Keep Import Selection using the unified
  node, with its existing empty-selection and pending-operation safeguards.
- Use the deformation-safe selection replay contract above, not unconditional
  raw-index acceptance. Keep topology/element-identity validation and active
  gesture safeguards distinct.
- Preserve older projects containing Captured Selection through legacy loading
  support or a verified conversion that retains selection data and socket links.
  Consolidating the UI must not simply remove the old node's registration.
- Verify manual selections, captured Point/Edge/Face selections, large captures,
  old-file compatibility, copying, undo/redo and save/reload. Update the wiki.

Implementation order: establish the replay/identity and pivot fixes first,
then consolidate the node and compatibility handling, then update Viewer edit
recipes/UI and run the combined regression checks.

## Next-phase task: Viewer UI simplification (2026-10-12)

**Status: implemented and verified on 2026-10-12 in the BEX-disabled Release build.**

Requested changes:

- Rename the current **Enable Editing** entry point to **Edit Mode**, retaining
  the ability to enter/exit the session and a clear indication of its state.
- Remove the separate **Enable Selection** control/workflow from the Viewer UI.
  Edit Mode already supports picking elements and **Import Selection**; retain
  that import action and its empty-selection/pending-operation safeguards.
- Remove all instructional/info labels previously shown between **Import
  Selection** and **Preview Offset**, including the G/S/R/E, X/Delete and Ctrl+R
  shortcut text and operation instructions in that span. Retain selection
  counts and unrelated validation warnings.
- Remove the **Preview Offset** feature and its numeric local-space X/Y/Z
  movement-preview workflow, including controls used only to confirm/cancel
  that preview. This supersedes the earlier instruction to keep it.
- Preserve native viewport movement (G and the move widget), including numeric
  input and viewport confirmation/cancellation. Keep the abort controls and
  transaction safeguards needed for extrusion, loop cuts and other operations;
  shared operation infrastructure must not be removed merely because it also
  supported Preview Offset.

Verify entering/exiting Edit Mode, element-domain picking, selection import,
empty-selection rejection, native movement/numeric input and pending-operation
controls after the UI cleanup.
The wiki now documents the revised UI and unified node, with a short Viewer
cleanup entry under Miscellaneous.

## Next-phase implementation record (2026-10-12)

- Select by Index has Manual and Captured modes. Manual keeps its existing index
  parser and geometry-independent Selection output; Points expands edge/face
  selections to their explicit vertices, and Valid reports syntax validity.
  Captured uses serialized element and affected-point arrays, a source-stage
  Geometry input, Selection, Points, Valid, and a Pivot output. All new Viewer
  imports and edit recipes use this node, regardless of selection size.
- Replay always checks element counts and ordered connectivity. Unique point
  `id` values additionally check ordered element identity. Without reliable IDs,
  uniquely distinguishable connected vertex incidences permit position-only
  changes; isolated/ambiguous geometry stays strict, and pure symmetric coordinate
  permutations are rejected. This is not general remapping across topology
  changes or arbitrary reorder-and-deform operations.
- Native active sessions retain exact geometry snapshots and context checks.
  Relaxed replay validation never relaxes in-progress gesture safety.
- Median and active-element pivots are recomputed from the capture input geometry.
  Cursor and explicit override pivots stay fixed. Scale Elements reads Pivot;
  matrix recipes subtract the pivot, apply the linear transform, and add it back.
- Legacy Captured Selection remains registered and preserves arrays, links and
  old strict validation on file load. Its old files contain no reliable identity
  baseline, so existing captures require recapture to gain deformation-safe replay.
  The legacy node is hidden from Add/search rather than destructively removed.
- Viewer now has Edit Mode / Exit Edit Mode, selection counts and guarded Import
  Selection. Separate selection-only and numeric Preview Offset controls are
  removed. Native G/S/R, numeric input, undo/cancel, topology operations and
  essential abort controls remain.

Verification completed on the same Windows x64 Release INSTALL profile with
`WITH_BEX=OFF`, Blender 5.3.0 Alpha, build timestamp 2026-10-12 11:52:17 UTC:

- All 13 headless suites pass: selection replay, pivot replay, editing, old-file
  compatibility, selection, transforms, extrusion, loop cuts, deletion/dissolve,
  layout, and the three recipe-contract suites.
- The original E -> S -> change Offset failure now passes without changing
  connectivity or membership, and subsequent G/R replay also passes.
- Pivot replay passes 24 Point/Edge/Face x Scale/Rotate x Median/Active/Cursor/
  Override cases, including node copying and an active-pivot save/reload case.
- Editing checks cover manual syntax and affected-point masks, all three capture
  domains, empty-import rejection, one-step undo/redo, strict active-session
  invalidation, connected deformation, same-count face reordering, symmetric
  coordinate permutations, isolated-point rejection, unique/reordered/duplicate
  IDs, a 2048-element capture, copying and save/reload.
- An actual `.blend` fixture created by the previous executable loads both legacy
  capture nodes and a storage-less manual Select by Index. Mode switching,
  copying and save/reload preserve its domain, indices and existing links. The
  loader initializes new manual-node storage before undefined-node validation.
- All six real event-loop suites pass with clean process exit: Editing,
  Transform, Extrude, Loop Cut, Delete and Node Selection. Native G/X/2 numeric
  input, confirmation/cancellation, modal lifetime, undo, file/quit cleanup and
  actual node clicks are exercised instead of the removed numeric preview API.
- Layout passes 20 checks, transforms 9 cases, delete/dissolve 25 checks, and
  transform/extrude/loop-cut recipe contracts 39 / 50 / 1013 checks.
- `BEX_DISABLED_FINAL_BUILD_OK` verifies that BEX is disabled, its three scripting
  node types are absent, and the custom selection/modelling nodes are registered.
  No BEX-enabled distribution was created.
- All 20 Viewer Python files parse, keymap routing passes 64 checks, wiki
  TypeScript no-emit and scoped C++ formatting/whitespace checks pass. GTests
  remain disabled in this profile; no full Blender test-suite claim is made.

Executable:
`C:\Users\rajee\Development\build_windows_x64_vc17_Release_no_bex\bin\Release\blender.exe`.
Build and test logs plus the older-build fixture are retained under that build's
`viewer_next_phase_validation` directory. Existing unrelated working-tree edits
were preserved; no commit or publication was performed.

## Phase 0 implementation (2026-10-11)

The Viewer has a separate **Enable Editing** entry point. Point/Edge/Face picking
uses the native temporary EditMesh, but unsupported native mesh/transform tools
are blocked in this mode so they cannot silently edit a disposable copy.

The minimal operation proving the transaction seam is **Preview Offset**. Its
dialog changes a local-space numeric offset and updates one pending Captured
Selection + Set Position pair. It freezes the picked domain, affected vertices,
active element and median pivot. **Cancel Edit** restores the previous Viewer
link and proxy positions; **Confirm Edit** retains the pair, removes the runtime
proxy, restores context and pushes one global graph-undo step. A zero offset
does not retain nodes or push a graph edit. Confirm currently exits the session;
automatic continuation belongs with the next interaction phase.

Only the Viewer input is rewired. Group Output and other consumers keep their
existing links. Connecting the confirmed branch to the final output is an
explicit user action, not an automatic fan-out rewrite.

Selections exceeding the old 1024-byte text limit import as **Captured Selection**.
Its serialized integer arrays store both the picked elements and explicit
affected points. Smaller manual imports still use Select by Index. The captured
node exposes Selection, Points and Valid and owns its arrays across copying,
undo, and file writes. Its field also validates the mesh on which it is consumed.

Validation tracks IDs, the modifier UID, Viewer/socket identifiers, the compute
context hash, frame and preview generation. Ordered connectivity **and ordered
positions** identify a capture's input stage; no evaluation-log, BMesh element
or node-cache pointers survive across updates. Position changes are deliberately
conservative invalidation, covering reordered isolated points and symmetric
meshes as well as connectivity changes. This is not a persistent identity system
and does not support animated/upstream-deforming captures without recapture.

Pending previews are cancelled on a lost source/context, Viewer deletion, editor
closure/switch, frame/input/proxy changes, disable, native Edit Mode exit, undo or
file load. Saving or quitting closes the session and discards unconfirmed work
before ordinary or quit-recovery file writes. Automatic saves wait until the
session ends rather than interrupting selection or serializing its preview.
Temporary selection changes and native Edit Mode exit do not push source-mesh or
pending-preview undo steps. Undo closes the session, and a post-undo sweep
removes only runtime IDs owned by past Viewer sessions if another editor's
memfile step resurrected them.

The shared implementation is in
[Viewer sessions](../source/blender/editors/space_node/node_geometry_viewer_selection.cc),
[stage-signature helpers](../source/blender/nodes/NOD_geometry_edit.hh), and
[Captured Selection](../source/blender/nodes/geometry/nodes/node_geo_captured_selection.cc).
[The Phase 0 smoke test](../tests/python/geometry_viewer_editing_smoke.py) exercises
the same operators as the UI.

### Validation results

Verified on 2026-10-11 in one Windows x64 Release build profile with `WITH_BEX=OFF`
(Blender 5.3 Alpha, local `bleeding-edge` working tree). The final `INSTALL` target
completed successfully. No BEX-enabled build was created for this phase.

- Factory-startup node registration passed.
- [Legacy selection import](../tests/python/geometry_viewer_selection_smoke.py)
  passed and exited cleanly: `GEOMETRY_VIEWER_SELECTION_SMOKE_OK`.
- [Editing transactions](../tests/python/geometry_viewer_editing_smoke.py) passed
  and exited cleanly: `GEOMETRY_VIEWER_EDITING_SMOKE_OK`. This covers empty-import
  rejection, Point/Edge/Face affected vertices, repeated previews, cancel/disable,
  zero offsets, one-step confirmation undo/redo, unchanged source/output branches,
  same-count invalidation, Viewer deletion/rewiring, 2048 scattered selected
  points, copying and save/reload.
- [Real viewport event loop](../tests/python/geometry_viewer_editing_ui_smoke.py)
  passed: `GEOMETRY_VIEWER_EDITING_UI_SMOKE_OK`. Timers allow real reevaluation
  between operations, checking frame/editor changes, source deletion, native
  Edit Mode exit, absence of a pending-preview undo step, and cleanup before
  quit recovery while a transaction is pending.
- [Disabled build option](../tests/python/bex_build_option_smoke.py) passed:
  `BEX_BUILD_OPTION_SMOKE_OK`; ordinary custom modeling nodes remain registered.
- The wiki TypeScript check passed without building the documentation site.

Verification fixed missing node RNA registration, cleanup before the initial Main
exists, guarded-allocator static lifetime at shutdown, and native mode-exit
operator filtering/undo ownership. The session registry uses function-local
statics, following Blender's allocator lifetime contract.

Viewport widgets/G/R/S, E extrusion and Ctrl+R are **not implemented by Phase 0**.
The numeric preview is the foundation test path, not native transform parity.

## Phase 1 source implementation (2026-10-11; not built)

The native Move/Scale/Rotate operators now enter the Viewer transaction through
the small [editor interface](../source/blender/editors/include/ED_geometry_viewer.hh).
Their existing gizmos, modal input, constraints, numeric entry and precision
handling are retained. Each update changes the pending recipe, not the original
mesh and not a replacement BMesh. Native transforms own the proxy until their
TransInfo/TransData are freed; only then may confirmation or cleanup retire it.

- Move keeps Set Position's current Position field and stores a local Offset.
- Representable edge/face uniform and single-axis scales use Scale Elements,
  the captured domain mask and a linked Vector value for the fixed pivot.
  Its Center socket's implicit Position field cannot be overridden merely by
  writing a socket default. Every selected island gets the same explicit pivot.
- Point scale, general XYZ/reflected/sheared scale and rotation use Position,
  Combine Matrix, Transform Point and Set Position. Matrix components use the
  existing Combine Matrix column/row sockets; no new transform evaluation node
  or context-dependent BMesh operator is introduced.
- Local replay is `inverse(M) * T * M`. The proxy is identity-parented to the
  source so its evaluated matrix preserves inherited shear, which decomposing
  the source matrix into an unparented proxy's channels would discard.
- Median Point, Active Element and 3D Cursor are supported. Unsupported pivots,
  trackball, mixed selection modes, proportional editing, Auto Merge, mirror
  and individual/rotate-aligned snapping are rejected. A singular object matrix
  is rejected. Preview and final replay are checked against the native vertices
  and fresh Viewer result with floating-point tolerance, plus ordered topology
  and the capture's upstream validation warnings.
- Recipe changes replace only transaction-owned helpers. Escape/no-op removes
  every pending node and restores the original link, without pushing undo.
- Confirmation removes runtime IDs before the native operator's one global
  undo step. On the next fresh notifier update, editing resumes from the
  authoritative Viewer result and restores selection, active element and hides.
  Each successive gesture gets a new input-stage capture. Undo/redo closes the
  proxy and restores the graph. The original numeric Preview Offset confirmation
  continues to exit editing as in Phase 0.
- The committed operator is converted to an internal graph-history entry with
  no native redo/repeat. This prevents Adjust Last Operation from transforming
  a retired proxy or the original object; edit the generated nodes instead.
- Explicit session termination cancels only the owned native modal handler,
  using its validated context, before freeing its BMesh. Unrelated modal tools
  are not cancelled.

Only the Viewer branch is rewired. Source mesh data, Group Output and sibling
geometry consumers retain their existing behavior. UV/custom-data correction
on the disposable native proxy is not exported as a separate attribute-edit
operation; the committed operation has the ordinary GN node's attribute semantics.
Phase 1 originally deferred E extrusion and interactive Ctrl+R. E is implemented
in the Phase 2 source notes below; interactive Ctrl+R is now implemented in source
as described in Phase 3 below, also without a new build.

### Phase 1 verification status

**No new Blender or documentation build was made for this update.**

- [Existing-node recipe test](../tests/python/geometry_viewer_transform_recipes_test.py)
  passed on the previously built Phase 0 executable:
  `GEOMETRY_VIEWER_TRANSFORM_RECIPES_OK 39`. It compares native ordinary-mesh
  transforms with node recipes for Point/Edge/Face, rotation, XYZ/reflected scale,
  world-axis scale, local-axis Scale Elements, disconnected islands, rotated
  objects, nonuniform scale and inherited parent shear. This verifies the math
  and existing kernels, **not** the new native Viewer integration.
- [Adapter smoke test](../tests/python/geometry_viewer_transform_smoke.py) is
  prepared for the next build. It covers stage continuation, capture domains,
  helper counts, world/local replay, no-op/rejection, unchanged source/output,
  and one-step graph undo/redo.
- [Real modal lifecycle test](../tests/python/geometry_viewer_transform_ui_smoke.py)
  is prepared for a GUI build with `--enable-event-simulate`. It covers repeated
  preview, confirm/Escape, automatic continuation, undo/redo, frame invalidation,
  save and quit during native transforms.
- The Phase 0 safety smoke test now expects native G/S/R to be available, while
  extrusion, topology mutations and edge slide remain blocked.

The Phase 1 compilation and interactive verification gate remains open until
those tests pass on a newly built executable.

## Phase 2 source implementation (2026-10-11; not built)

The default Blender Mesh keymap now routes **E** to the session-only
`NODE_OT_geometry_viewer_edit_extrude` before the ordinary native wrapper. Outside
Viewer authoring its poll is false, leaving the original key binding intact.
Custom keymaps can bind Viewer Extrude explicitly. Native context-bound mesh
extrusion operators remain blocked in authoring mode; toolbar/Alt+E/manifold and
shrink/fatten variants are not silently redirected.

The tool shares the existing transaction implementation:

- Capture the **original** stage's domain selection and incident points, create
  one Extrude Mesh node, and explicitly link an Extrusion Offset vector. The
  default Offset input is a Normal field; setting its socket default alone
  would not correctly record zero or a common movement vector.
- Choose Vertices, Edges, or Faces from the single active selection domain.
  Face region extrusion explicitly uses `Individual=false`. The operator's
  separately declared Individual Faces option uses the existing individual
  topology kernel with a shared captured movement, not per-face normal
  shrink/fatten. The normal E shortcut keeps region extrusion as its default.
- Consume Top through a temporary boolean Store Named Attribute helper on the
  evaluated result. Its private name is chosen not to overwrite an input
  attribute. The helper is removed before commit, and no private picking
  attribute is part of the durable geometry stream.
- Evaluate the **zero-offset** topology, rebuild the runtime proxy at this safe
  stage (before any native TransInfo exists), and select its actual Top elements.
  Keep the pre-extrusion capture/signature separate from this output-stage
  topology, baseline positions and frozen Top membership.
- Start the native Translate gesture. Face mode starts on the native Normal Z
  axis; point/edge modes start unconstrained. Update the same Offset vector from
  the inverse source linear transform of the captured world displacement.
  Subsequent previews replay from the original geometry, not an already
  extruded preview. Node geometry and native Top movement must agree.
- Reject non-translation mode switches during this topology-and-movement step,
  as well as the existing unsupported proportional/mirror/Auto Merge/snapping
  options. Confirming creates one global undo step after retiring the proxy;
  resumption selects Top on the evaluated result, supporting E→G→S→E chains.
- **Esc/RMB cancels movement only**, updating Offset back to zero while keeping
  extrusion topology and its one undo step. **Shift+Esc** or **Abort Entire
  Extrusion** removes the whole pending operation and exits authoring. Explicit
  disable, invalid input/context, native handler removal, save/load and close
  forcibly abort rather than creating a zero-offset extrusion accidentally.

No native mesh extrusion operator is invoked from node evaluation. The source
mesh, Group Output and sibling branches are not modified. The committed recipe
is ordinary Captured Selection + Extrude Mesh + Vector nodes, with Top and Side
still available for downstream use. Native transform ownership is released
before deleting or rebuilding the proxy. Gizmo-dispatch polling also permits
the existing safe transform wrapper while preventing parallel gestures.

### Declared extrusion limits

Existing Extrude Mesh topology/attribute semantics are authoritative. In
particular, region mode reuses selected cap faces and some inner vertices,
duplicates boundary geometry and adds sides. It is not a promise of every
native region/back-face/manifold behavior. Selections that produce no new
topology or no Top elements are rejected (including a completely selected
closed surface in region mode). Mixed selection domains and singular source
object transforms are also rejected. UV/attribute correction options of a
native transform are not separately authored.

### Phase 2 verification status

**No new Blender or documentation build was made for this update.**

- [Existing-kernel replay test](../tests/python/geometry_viewer_extrude_recipes_test.py)
  passed on the existing Phase 0 executable:
  `GEOMETRY_VIEWER_EXTRUDE_RECIPES_OK 50`. Fixtures cover point/edge/region/individual
  face topology, connected/disconnected regions, repeated offset changes and
  return to zero, rotated/nonuniformly scaled and inherited-shear objects,
  UV corners and material indices. Native translation on the GN zero-offset
  Top matches the captured-offset replay. This is **not** a test of native
  extrusion topological parity or the new Viewer adapter.
- [Prepared adapter/chain smoke test](../tests/python/geometry_viewer_extrude_smoke.py)
  checks empty/domain rejection, durable node recipes, Top resumption, unchanged
  source/output, E→G→S→E, zero-offset commit and one-step undo/redo. It needs a
  newly built executable and has **not** been run.
- [Prepared real-input/lifecycle smoke test](../tests/python/geometry_viewer_extrude_ui_smoke.py)
  checks the E key, repeated preview without accumulated geometry, nonzero and
  Esc's zero-offset commits, explicit/Shift+Esc whole-operation abort, undo/redo,
  frame invalidation and save during native ownership. It needs a Phase 2 GUI build and has **not** been
  run. Both its success marker and clean process exit are required.
- [Default-keymap test](../tests/python/geometry_viewer_edit_keymap_test.py)
  passed without Blender: `VIEWER_EDIT_KEYMAP_OK 16`. It checks Viewer-first E
  routing while retaining the native operator/tool and Alt+E bindings across
  left/right selection, legacy, fallback-tool and tool-activation settings.

Compilation and the Phase 1/2 adapter, modal and lifecycle verification gates
remain **open**. The subsequent Ctrl+R source work is recorded below; none of
these unbuilt integrations is being claimed as runtime-validated.

## Phase 3 source implementation (2026-10-12; not built)

The default Mesh keymap now places `NODE_OT_geometry_viewer_edit_loop_cut`
before ordinary Ctrl+R. The session-only poll leaves ordinary mesh loop cutting
unchanged outside Viewer editing. Custom keymaps can bind **Viewer Loop Cut and
Slide** explicitly. Native `mesh.loopcut_slide`, edge-slide and offset-loop tools
remain blocked on the disposable authoring proxy; they are not silently adapted.

### Shared kernel contract and live preview

The hovered crossing edge, not the existing picked membership or Alt-click loop,
is captured on the **Edge** domain at the original input stage. A new lightweight
`preview_loop_cut_and_slide_mesh` entry point shares the existing kernel's
quad-opposite traversal, accepted components, consistent seed orientation and
the exact cut fraction `(cut + 1 + Factor) / (Cuts + 1)`. It returns only preview
line endpoints and skipped-ring count; it does not subdivide or convert to BMesh.
The geometry kernel itself continues to use its bounded BMesh conversion, as
before. No context-bound native topology operator is called during evaluation.

Only one Captured Selection and one Loop Cut and Slide operation are pending.
Hover/cut/factor changes update these nodes and always replay their original
input. A temporary boolean Store Named Attribute consumes **Loops** for result
picking and is removed before commit; its private attribute name avoids input
attribute collisions. Invalid candidates show a red hovered edge and status
explanation; they cannot be placed. Hovering away from a valid ring clears the
capture mask without retaining an unintended cut or creating another node.

The original EditMesh is the picking snapshot through both modal stages. It is
never subdivided/replaced underneath an active tool; the colored loop overlay
and evaluated Viewer branch preview the authoritative operation. Result topology
and selection come from actual evaluation, not guessed BMesh index correspondence.
The existing transaction now shares its temporary-field cleanup and post-undo
resume seam across transforms, extrusion and loop cutting.

### Two stages and cancellation

1. **Choose cuts:** mouse hover selects the ring; wheel, Page Up/Down or typed
   count sets 1–100 Cuts. Factor stays zero. LMB/Enter enters sliding. Esc/RMB
   removes the whole pending cut and preserves the original picking mesh and
   selection, without a graph undo step.
2. **Slide:** mouse displacement along the world-projected stored seed edge
   changes signed Factor, clamped to -1…1. One factor unit is the projected seed
   length divided by Cuts+1. Shift applies precision; typed Factor remains
   available when the edge projects end-on. LMB/Enter confirms.
3. **Center or abort:** Esc/RMB during sliding commits centered cuts at Factor=0,
   including one graph undo step. Shift+Esc during sliding and **Abort Entire
   Loop Cut** remove all pending nodes and exit the session. Disable Editing,
   frame/input/context changes, save/load and shutdown forcibly abort both
   stages rather than committing centered cuts as a side effect.

Before the operator's global undo snapshot, the transaction disowns the durable
Capture/Loop Cut nodes and retires the original runtime IDs. On the next refresh,
a fresh proxy is created from the committed evaluated Mesh, with **Loops** as
its active Edge selection. Subsequent transforms/extrusion capture this new
stage. **Loop Index** remains an ordinary downstream output. The original mesh,
Group Output and unrelated upstream consumers remain untouched.

### Declared loop-cut limits

This adapts the current node's orientable quad-ring contract, not every native
Loop Cut/Edge Slide mode. No-quads, non-manifold branching and inconsistent ring
orientation are rejected. Cuts remain inside the node's displayed 1–100 range;
Factor is clamped. Even/Flipped, smoothing/falloff, unclamped/offset sliding,
toolbar tools and persistent topology IDs remain outside this phase. **Reveal
locally hidden elements before cutting**: local hide flags are not remapped
through subdivision, and the tool rejects such a session rather than reusing
stale element indices. Mixed selection modes are rejected; a single Point,
Edge or Face picking mode is supported, independently of the hovered Edge seed.

### Phase 3 verification status

**No new Blender or documentation build was made.**

- [Existing-kernel replay tests](../tests/python/geometry_viewer_loop_cut_recipes_test.py)
  passed on the older Phase 0 executable:
  `GEOMETRY_VIEWER_LOOP_CUT_RECIPES_OK 1013`. Every seed of a non-planar quad
  strip and closed cylinder is checked through changing cut counts and positive,
  negative, endpoint and zero factors. Tests check the independently computed
  oriented loop edges, Loops/Loop Index, stable topology during sliding,
  point-attribute/UV interpolation, triangle/wire/non-manifold rejection and
  empty selection. This validates the **existing kernel**, not the new adapter
  or new C++ preview entry point.
- [Default-keymap tests](../tests/python/geometry_viewer_edit_keymap_test.py)
  passed: `VIEWER_EDIT_KEYMAP_OK 32`, covering Viewer-first E and Ctrl+R plus
  retained native/tool bindings across selection, legacy, fallback and tool
  activation preferences.
- [New shared C++ preview tests](../source/blender/geometry/tests/GEO_mesh_loop_cut_slide_test.cc)
  compare preview lines to actual Loops edges for every grid seed, signed factors,
  multiple cuts, intersecting rings and empty/wire seeds. They are prepared but
  **not compiled or run**.
- [Prepared adapter/chain test](../tests/python/geometry_viewer_loop_cut_smoke.py)
  checks seed-vs-domain independence, invalid seed/hidden-element guards, durable
  nodes, actual Loops resumption, inherited-shear transforms, cut→move→scale→cut→E,
  source/output preservation, one-step undo/redo and save/reload replay. It has
  **not been run** and needs a Phase 3 build.
- [Prepared real-input/lifecycle test](../tests/python/geometry_viewer_loop_cut_ui_smoke.py)
  checks actual Ctrl+R/hover, wheel/typed count and a typed negative factor, previews on one node,
  untouched picking topology, ordinary placement/slide cancellation, nonzero
  confirmation, explicit/Shift+Esc whole-operation abort, edge selection after
  resumption, undo/redo, frame invalidation and saving during slide. It has
  **not been run** and requires a Phase 3 GUI build. Require its marker and a
  clean process exit.
- Python syntax checks (`VIEWER_EDIT_PYTHON_SYNTAX_OK 14`), scoped diff/format
  checks, and the wiki's TypeScript no-emit check passed. These are source checks,
  not evidence of compilation or runtime correctness of the new C++ adapter.

The preceding verification notes record the source-only Phase 3 snapshot.
Compilation and newer adapter/modal checks are recorded under Phase 4 below;
these do not constitute a general production-release or full-test-suite claim.

## Phase 4 subset: Delete / Dissolve (2026-10-12)

Bevel is deferred at the user's request. **X** and **Delete** now invoke the
Viewer-first **Delete / Dissolve** menu, while ordinary edit meshes retain their
native menu. Opening/cancelling the menu has no transaction side effects; the
capture is created only when the user chooses an action.

| Menu action | Capture domain | Durable operation |
| --- | --- | --- |
| Vertices | Point | Delete Geometry, All |
| Edges | Edge | Delete Geometry, All |
| Faces | Face | Delete Geometry, All |
| Only Edges & Faces | Edge | Delete Geometry, Only Edges & Faces |
| Only Faces | Face | Delete Geometry, Only Faces |
| Dissolve Vertices | Point | Dissolve Elements, Point |
| Dissolve Edges | Edge | Dissolve Elements, Edge |
| Dissolve Faces | Face | Dissolve Elements, Face |

The explicit action chooses the capture domain, independently of the picking
domain. Native selection flushing supplies incident vertices/edges of selected
faces. Capture Selection is linked to the operation, with geometry from the same
input stage. Only Viewer is rewired; source data, Group Output and siblings remain
unchanged. There is no context-bound native mesh operator in evaluation.

The existing transaction seam now permits a commit without resuming a proxy when
the entire mesh is deleted. Otherwise it retires runtime IDs before one global
undo snapshot and resumes from the evaluated result, retaining the picking domain
but **clearing selection and active history**. Delete and Dissolve have no result
selection output; guessing old indices on reordered topology is unsafe.

Empty selections and no-op dissolve candidates leave no nodes/graph undo step.
Pending edits must be confirmed or cancelled first. Reveal locally hidden
elements before deleting/dissolving; remapping hide state is not implemented.
Mixed selection modes are rejected. Native Limited Dissolve, Ctrl+X/Delete,
angle/delimiter settings and every Edit Mode delete variant are not adapted.
Loose-geometry cleanup and dissolve topology/attribute behavior are exactly those
of the existing nodes; these adapters do not silently change their kernels.

Evaluation is bracketed by a synchronous preparation flag and live-ID validation.
The capture arrays/signature, operation domain/mode and expected graph links must
still match; node warnings or lost context abort owned nodes safely.

### Verification

- One Windows x64 Release `WITH_BEX=OFF` `INSTALL` build completed successfully.
  Its executable is `../build_windows_x64_vc17_Release_no_bex/bin/Release/blender.exe`
  relative to the repository root (Blender 5.3.0 Alpha, source hash `972e506154c8`
  plus uncommitted feature changes; final build banner `2026-10-11 21:09:18`).
  No BEX-enabled build was created. The final executable reports BEX disabled
  (`BEX_DISABLED_FINAL_BUILD_OK`).
- Keymap checks passed: `VIEWER_EDIT_KEYMAP_OK 64`, including ordinary native
  fallback across 16 preference combinations for E, Ctrl+R, X and Delete.
- [Adapter smoke](../tests/python/geometry_viewer_delete_smoke.py) covers all eight
  recipes against ordinary-node references, attributes/UVs, empty/no-op/pending/
  hidden guards, explicit-action vs picking domain, cut→dissolve→delete,
  one-step undo/redo, save/reload and complete mesh deletion. Passed with
  `GEOMETRY_VIEWER_DELETE_SMOKE_OK 25` and clean process exit. Background Python
  operator calls require explicit test undo snapshots; the popup test below
  separately verifies automatic GUI undo recording.
- [Real popup smoke](../tests/python/geometry_viewer_delete_ui_smoke.py) covers X
  and Delete, Escape, numeric action selection, empty selection, undo/redo and
  ordinary edit-mesh fallback. Passed with
  `GEOMETRY_VIEWER_DELETE_UI_SMOKE_OK` and clean process exit.
- Background regressions passed: `GEOMETRY_VIEWER_TRANSFORM_SMOKE_OK 9`,
  `GEOMETRY_VIEWER_EXTRUDE_SMOKE_OK`, `GEOMETRY_VIEWER_LOOP_CUT_SMOKE_OK`,
  `GEOMETRY_VIEWER_EDITING_SMOKE_OK`, and `GEOMETRY_VIEWER_SELECTION_SMOKE_OK`.
- Ordinary-node recipe checks passed: `GEOMETRY_VIEWER_TRANSFORM_RECIPES_OK 39`,
  `GEOMETRY_VIEWER_EXTRUDE_RECIPES_OK 50`, and
  `GEOMETRY_VIEWER_LOOP_CUT_RECIPES_OK 1013`.
- Real GUI tests pass with clean process exits:
  `GEOMETRY_VIEWER_TRANSFORM_UI_SMOKE_OK`,
  `GEOMETRY_VIEWER_EXTRUDE_UI_SMOKE_OK`,
  `GEOMETRY_VIEWER_LOOP_CUT_UI_SMOKE_OK`,
  `GEOMETRY_VIEWER_DELETE_UI_SMOKE_OK`, and
  `GEOMETRY_VIEWER_EDITING_UI_SMOKE_OK`. The transform lifecycle includes frame invalidation,
  save and quit while a transform owns the proxy. Invalidation retires runtime
  IDs after cancelling the modal handler; tagged dependencies are refreshed
  before drawing so evaluated meshes cannot retain a deleted original BMesh.
- Initial E/Ctrl+R fixture failures were caused by the startup splash consuming
  simulated input. UI tests explicitly dismiss it before testing viewport keys.
  Extrusion modal testing additionally exposed
  Shift+Escape being remapped by the transform modal keymap; the adapter now
  checks the original event. Both zero-offset Escape and full Shift+Escape abort
  pass in the final build, along with repeated extrusion, undo/redo and file lifecycle.
- Python syntax, scoped C++ formatting and whitespace, wiki TypeScript no-emit,
  and keymap checks pass.
  The separate C++ GTests have not run (`WITH_GTESTS=OFF`); no full-suite or
  arbitrary-mesh/native-tool-parity claim is made.

The next authorized subset, **Merge / Duplicate and Move**, is recorded below.
The other Phase 4 items remain proposed; bevel remains deferred.

## Phase 4 subset: Merge / Duplicate and Move (2026-10-12)

**Historical checkpoint: implemented in source, without a build at that time.** The user
initially requested implementation and a build, then explicitly changed the
scope to implementation only. The existing BEX-disabled executable still
contained the previously built node-layout phase, not these new adapters. The
later face-tools build below supersedes that executable and verification status.

### Merge by Distance

- The default Mesh keymap routes **M** to a session-only Viewer settings dialog
  before the ordinary Merge menu. Choose All, Connected or Not Connected and a
  mesh-local Distance. The dialog owns no transaction until confirmation;
  cancellation changes no nodes or mesh data. Custom keymaps may bind
  `node.geometry_viewer_edit_merge` directly.
- Confirmation inserts validated **Select by Index (Captured) -> Merge by
  Distance -> Viewer**. The capture retains the picking domain; the operation
  uses **Points** so selected edges/faces affect their incident vertices. Existing
  merge kernels and all three node modes are unchanged.
- Empty selection, fewer than two affected points, non-finite distances and
  no-op merges do not commit a recipe or graph undo step. Locally hidden geometry
  and mixed selection domains are rejected before node insertion; hidden-element
  remapping through welding is not guessed.
- The operation is evaluated and its context, capture, links, controls and
  warnings revalidated before commit. It retires the temporary mesh before one
  global graph undo snapshot and queues editing on the actual result, retaining
  the picking domain. Selection and active history are cleared because Merge by
  Distance has no authoritative output-selection field.
- Merge-to-center/cursor/active and unselected-target variants are separate work,
  not mislabeled as distance merging. The source mesh, Group Output and sibling
  branches remain unchanged.

### Duplicate and Move

- **Shift+D** invokes `node.geometry_viewer_edit_duplicate` only for an active
  Viewer editing session; ordinary mesh duplication remains the fallback.
- The permanent connected-region recipe is **Select by Index (Captured) ->
  Separate Geometry -> Set Position**, with the moved copy and untouched source
  passed to **Join Geometry -> Viewer**. Separate Geometry uses the picking
  domain and its existing All-removal topology rules. Duplicate Elements is
  intentionally not used: it isolates face/edge elements rather than preserving
  connected-region boundaries like native mesh duplication.
- A collision-free, temporary boolean Store Named Attribute marks only the
  copied domain. The result is evaluated once at zero offset and those actual
  elements are selected on the proxy before the native transform starts. The
  existing extrusion topology synchronization was extracted into a shared helper;
  it still runs only before TransInfo owns the BMesh.
- Native global movement, constraints, typed input and precision update a single
  Set Position Offset in mesh-local space. The copy is never duplicated again
  from a previous preview. The original captured selection and affected-point
  arrays are kept separately from the picked result arrays.
- **Esc/RMB cancels movement only**, retaining a zero-offset copy and its single
  undo step. **Shift+Esc / Abort Entire Duplication** removes the whole pending
  recipe and exits editing. Exit, invalid frame/input/context, save/load and close
  forcibly abort, rather than committing coincident copies accidentally.
- Commit removes the picking helper and reconstructs Join inputs in their
  original order. Its two multi-input sources, order and multiplicity are checked
  while pending; unexpected graph rewires are not accepted. Resume uses the Join
  result and selects only the copied domain for subsequent editing.
- Generated nodes remain width-spaced left-to-right, including their final Join
  and Viewer. Empty selections, hidden elements, mixed domains and singular object
  transforms are rejected. Existing proportional/mirror/Auto Merge and unsupported
  transform-mode safeguards remain in effect.

### Verification boundary

Run without rebuilding, using the previous BEX-disabled executable:

- `GEOMETRY_VIEWER_DUPLICATE_RECIPES_OK`: **44** checks comparing existing-node
  recipes with ordinary mesh duplication, with Point/Edge/Face regions,
  connected adjacent faces, disconnected faces, loose points/edges, zero and
  nonzero local movement, material/attribute/UV preservation, and unchanged
  ordering with the temporary picking attribute.
- `GEOMETRY_VIEWER_MERGE_RECIPES_OK`: **13** checks for incident-point expansion,
  all three existing merge modes, same-island protection and unselected points.
- `VIEWER_EDIT_KEYMAP_OK`: **96** checks across left/right select, fallback-tool,
  legacy and tool-activation variants; native M and Shift+D fallback order is
  retained together with E/Ctrl+R/X/Delete routing.
- Python AST parsing passes for all **26** Viewer test scripts. Scoped C++
  formatting/whitespace checks and wiki TypeScript `--noEmit` pass. These checks
  do not compile or validate the new C++ viewport adapters.

Prepared **but not run** because they require a build containing this source:

- [Adapter smoke test](../tests/python/geometry_viewer_merge_duplicate_smoke.py):
  all merge modes/domains, no-op and hidden/empty guards, captured Points routing,
  copied-domain continuation, node layout, zero/nonzero world movement on a
  nonuniformly transformed object, undo/redo, duplicate -> scale -> duplicate ->
  delete chains and save/reload replay.
- [Merge GUI test](../tests/python/geometry_viewer_merge_ui_smoke.py): actual M
  dialog cancellation/confirmation, no-op confirmation without extra undo,
  deselected post-merge continuation, one-step undo/redo and save cleanup.
- [Duplicate GUI test](../tests/python/geometry_viewer_duplicate_ui_smoke.py):
  actual Shift+D, repeated previews, connected faces and result selection,
  Escape zero-offset commit, typed axis movement, button/Shift+Escape whole-copy
  abort, one-step undo/redo, frame invalidation and saving during movement.

The previously built extrusion, transform, loop-cut, deletion, selection replay
and node-layout suites must also be rerun with that future build, because this
subset shares the topology-sync and native-transform lifecycle seams. No
compiler, GTest or full native-operation-parity result is claimed for new source.
The wiki distinguishes this source-only subset from features already compiled.
No commit, publication, executable replacement or BEX build was performed.

## Phase 4 subset: selected face tools (2026-10-12)

**Status: implemented, compiled and verified in one BEX-disabled Windows build.**
This is the next bounded existing-node subset after Merge and
Duplicate. Bevel remains deferred. Material assignment, split/separate streams,
selected attribute writes and the Phase 5 topology tools remain proposed.

- **Ctrl+T** authors captured Select by Index plus ordinary Triangulate, with
  Beauty/Beauty defaults matching the native shortcut. **Ctrl+Shift+T** uses
  Fixed/Clip. Both methods are explicit Menu inputs and editable afterward.
- **Ctrl+F** opens a session-only face-tools menu with Triangulate, Flip Faces,
  Shade Smooth and Shade Flat. Menu cancellation does not allocate a transaction.
  Default Mesh keymaps retain ordinary native fallbacks outside Viewer editing.
- This subset requires **Face selection mode**, with nonempty selected faces.
  Already-triangular selections and already-applied shading produce no recipe
  or graph undo step. Normal recalculation/custom normals are not implemented.
- Triangulate temporarily stores each input face index as an integer Face
  attribute before the existing node kernel, using a collision-free name. Its
  result must carry a complete face lineage with exactly `size - 2` children for
  each selected polygon and one output per unselected face. All children of the
  picked faces become selected; one child of the original active face becomes
  active. Missing/ambiguous correspondence rolls back, not guessed raw indices.
- Both mapping helpers are removed and the ordinary source-to-Triangulate link
  restored before commit. No mapping nodes/private attributes are retained in
  the durable graph. Triangulate rejects local hiding rather than reinterpreting
  stale edge/face indices. Vertex order and affected-point membership are kept.
- Flip Faces reverses selected face winding. Smooth/Flat uses Set Shade Smooth
  in Face domain with an explicit Boolean. These operations retain face indices,
  selected membership, active face and all hiding. Result positions, edge indices
  and face offsets are checked before reusing that correspondence.
- All operations use the same validated capture, owned-node rollback, scene/frame
  checks, left-to-right layout and retire-before-global-undo lifecycle. Editing
  resumes on the actual result, with the mapped selection. Group Output, original
  mesh data and sibling branches remain untouched.

Verification scripts:

- [Face adapter/native parity](../tests/python/geometry_viewer_face_tools_smoke.py):
  all five quad methods and both n-gon methods, concave polygons, winding, UVs,
  materials, attributes, loose topology, node replay, undo/redo, wrong/empty
  domain, no-op and hiding guards, mixed operation chains and persistence.
- [Real face-tool shortcuts](../tests/python/geometry_viewer_face_tools_ui_smoke.py):
  Ctrl+T and Ctrl+F menu actions/cancellation, triangulated face lineage and active
  history, continued movement, no-op undo suppression and save cleanup.
- Default keymap validation now covers **144 checks** across 16 configurations,
  including native Ctrl+T/Ctrl+Shift+T/Ctrl+F fallback order.

Runtime verification on the built executable: **Blender 5.3.0 Alpha**, built
2026-10-12 18:33:41 UTC (2026-10-13 00:03:41 IST), `bleeding-edge` modified
working tree at `972e506154c8`. The Windows executable remains
`build_windows_x64_vc17_Release_no_bex/bin/Release/blender.exe` in the sibling
build directory beside the source checkout.

Runtime checks:

- `GEOMETRY_VIEWER_FACE_TOOLS_SMOKE_OK`: **25** scenarios, including native parity
  with all five quad and both n-gon methods, concave/nonplanar faces, selected
  flipping/shading, UV/material/attribute preservation, node replay, guards,
  undo/redo and mixed-chain save/reload.
- `GEOMETRY_VIEWER_FACE_TOOLS_UI_SMOKE_OK`: real Ctrl+T/Ctrl+F input, popup cancel,
  actual child-face selection and original active-face lineage, smooth/flat
  continuation, local hiding, no-op actions, movement and five-step undo/redo.
- `GEOMETRY_VIEWER_MERGE_DUPLICATE_SMOKE_OK`: **21** scenarios for the previously
  source-only Merge/Duplicate adapters. Existing-node recipe checks also pass
  again: **44** Duplicate and **13** Merge checks.
- Real Merge and Duplicate GUI tests pass, including M confirmation/cancellation,
  no-op suppression, Shift+D zero-offset Escape, typed movement, full abort,
  repeated copying, one-step undo/redo, frame invalidation and save cleanup.
- Earlier selection/replay/pivot/legacy-capture, transform, extrusion, loop-cut,
  deletion, node-selection and node-layout regression scripts pass. The old
  Delete test now recognizes `ABORT_DUPLICATION`; simulated typed characters are
  supplied only on key presses. These were test-harness assumptions, not changes
  to the existing mesh kernels.
- The full scoped run covers **17 headless** and **10 real-input GUI** scripts,
  requiring success markers and clean exits. Keymap checks pass (**144**);
  all **28** Viewer Python scripts parse; scoped C++ formatting/whitespace and
  wiki TypeScript no-emit validation pass.

`WITH_BEX=OFF` is confirmed both in the cache and by absent Geometry,
Compositor and Shader BEX RNA node types. Existing Select by Index, Loop
Selection, Loop Cut and Slide, Dissolve Elements, Viewer and IBK node types
and the new editor operators are present. `WITH_GTESTS=OFF`. No C++ GTest, full Blender suite or
arbitrary-mesh/native-operation-parity claim is made. Only the existing
BEX-disabled Windows install profile was rebuilt; no commit or publication was
performed. Material assignment, split/separate and attribute editing remain
separately scoped; bevel is still deferred.

## Viewer foundation before Phase 0: research snapshot

Before the Phase 0 changes, the selection operator already:

- Finds the Viewer mesh in contextual Geometry Nodes evaluation logs.
- Copies it into runtime Mesh/Object IDs, applies the source object's transform,
  pins the node editor and enters actual mesh Edit Mode on that temporary object.
- Reads selected point/edge/face indices from its BMesh, creates Select by Index,
  then ends the session and restores the original context.
- Rejects empty imports and changed vertex/edge/face counts.

At that snapshot it did **not** translate transforms or topology edits into nodes,
insert an operation on the Viewer link, or maintain a live authoring chain. Its
topology guard compared counts only; equal counts do not prove unchanged element
order or connectivity. Imported indices also pass through a fixed 1024-byte
string socket, so large scattered selections need a more scalable recording
path. Phase 0 addresses the transaction, signature and recording limitations.
Phases 1–3 add viewport transforms, extrusion and loop cut/slide as documented
above; current changes and verification results belong to the next-phase record
rather than the earlier source-only snapshots.

Sources: [session state](../source/blender/editors/space_node/node_geometry_viewer_selection.cc#L62),
[Viewer lookup](../source/blender/editors/space_node/node_geometry_viewer_selection.cc#L115),
[proxy creation](../source/blender/editors/space_node/node_geometry_viewer_selection.cc#L363),
[count guard](../source/blender/editors/space_node/node_geometry_viewer_selection.cc#L291),
[import and cleanup](../source/blender/editors/space_node/node_geometry_viewer_selection.cc#L462),
[string capacity](../source/blender/makesdna/DNA_node_types.h#L2137),
[existing smoke test](../tests/python/geometry_viewer_selection_smoke.py).

## Proposed user-facing workflow

1. Activate a mesh Viewer and choose **Edit Mode**. Viewport picking and Import
   Selection now share this single entry point.
2. While editing, show Exit Edit Mode, Point/Edge/Face selection controls and
   the selection count. Do not expose a second adjacent attribute-domain menu
   as though it controlled mesh editing. The 1/2/3 shortcuts and toolbar should
   keep the active edit domain synchronized.
3. Select and use widgets or supported shortcuts. A gesture starts one pending
   operation, with its selection already captured. Nodes appear once; dragging
   changes their parameters, not their number.
4. Confirm to retain the graph operation and continue selecting its evaluated
   result. An ordinary canceled transform restores the prior graph and geometry.
   Extrusion and loop-cut movement cancellation follow the explicit stage rules
   below. Undo removes the complete modeling gesture, including helper nodes.
5. Exit Edit Mode to discard the temporary interaction object while retaining
   confirmed nodes. Re-entering creates a fresh proxy from the graph result.

Keep unsupported topology-changing shortcuts out of the proxy while recording.
An unsupported tool must explain the limitation; it must not silently edit the
temporary mesh and lose that edit when the session ends.

### Graph placement and output routing

The requested placement is the authoring anchor:

```text
Before:   Upstream geometry --------------------------> Viewer

After:    Upstream geometry --> Operation ------------> Viewer
                               ^
                   Select by Index (Captured)

Later:    Upstream --> Move --> Extrude --> Loop Cut --> Viewer
```

Selection is captured against the exact geometry **entering** its operation.
Normal changes to that operation's parameters then reevaluate it procedurally.
Helper nodes sit next to the operation, preserve frames/parents and receive
readable labels. Leave unrelated output sockets and other branches alone.

A Viewer is not a modifier output. Default authoring should change only the
chosen Viewer input link. Offer an explicit **Use Edited Branch in Output**
action or selected output-target setting when the artist wants the chain to
drive Group Output. If the output path is ambiguous or already has additional
processing, require a target choice rather than replacing it automatically.

## Roadmap and release gates

| Phase | Deliverable | Gate before proceeding |
|---|---|---|
| 0 | Authoring session, selection capture, graph transactions and proxy synchronization | Begin/update/confirm/cancel survives context changes and undo; no source-mesh or unrelated-link edits |
| 1 | Move, scale and rotate from widgets and G/R/S | Node result matches supported native transforms, including coordinate spaces and explicit pivots |
| 2 | E extrusion with region/individual/domain modes | Chained extrusion works; movement cancel, generated selections and attribute behavior are specified and tested |
| 3 | Ctrl+R hover, wheel cuts and slide | Both modal stages behave predictably; subsequent edits target the new loops |
| 4 | Existing-node modeling toolkit | Bevel/delete/dissolve/merge/duplicate adapters preserve their declared edit semantics |
| 5 | Missing selection-aware topology operations | Inset/bridge/existing-edge slide/selected subdivide are individually validated, not approximated silently |
| 6 | Advanced interaction and modeling | Proportional editing, snapping, symmetry and deformation replay from explicit data |
| 7 | Broader contexts and release hardening | Nested/shared contexts, supported geometry types, performance and persistence meet documented limits |

Safety, tests and performance checks apply to **every** phase. Phase 7 is not
permission to defer reliable undo, cancellation or cleanup until the end.

### Phase 0: make the editing session reliable

- Track source Object, modifier, node tree, Viewer identifier, geometry socket,
  compute context, source revision and operation generation; never retain stale
  evaluation-log or BMesh element pointers across reevaluation.
- Begin with one realized mesh in one modifier context. Reject unresolved
  instances, mixed editing targets, zones and ambiguous Viewer geometry inputs
  with useful messages. Do not silently realize instances.
- Freeze the pre-operation selection, geometry and active element/pivot data.
  Retain both the picked domain and its explicit affected-point selection for
  transforms. Introduce structured index storage for large captured selections
  while preserving the existing human-authored Select by Index text workflow.
- Strengthen validity beyond element counts: a selection belongs to an input
  stage and topology/order signature. When upstream topology changes, warn and
  invalidate affected captures rather than retargeting them by coincidence.
- Keep lineage work bounded. Existing indices are sufficient for an unchanged
  stage; controlled topology kernels can later emit origin/new-element maps.
  A copied generic ID attribute is not a globally unique identity guarantee.
- Implement one graph transaction per gesture: create pending nodes/links,
  update values, confirm one undoable operation, or restore the previous state.
- Define native mesh-undo versus graph-undo ownership explicitly. Runtime IDs
  are excluded from ordinary file writes but can participate in undo; the
  session registry must reconcile restored IDs rather than assuming its static
  map survives undo/load correctly.
- Pin the source context; safely handle object deletion, Viewer deletion,
  editor closure, disabling the mode, undo/redo and file load. Preserve old
  selection-only import and empty-import rejection.

**Gate:** a minimal pending-node transaction can commit and roll back repeatedly,
including forced invalidation, without orphan proxies, stale links or crashes.

Runtime-ID persistence source:
[file writer](../source/blender/blenloader/intern/writefile.cc#L1755).

### Phase 1: transforms, matching the selected elements

**Move:** create Select by Index/captured selection plus Set Position. Leave
Position at its current-position field and update Offset from the gesture. Edge
or face selections expand to incident vertices. Do not assign one absolute
Position to a multi-point selection.

**Scale:** use Scale Elements for face/edge uniform and representable single-axis
operations, with a deliberately captured Center and Axis. Do not inherit its
default center and call that native Median Point parity. Point scaling, general
XYZ scaling, reflection and transforms that cannot be represented by its scalar
controls need an inspectable selected-transform helper recipe. Prefer existing
Position, matrix/vector utility and Set Position nodes first; a new compact
selected-transform node is a later design choice, not a prerequisite assumed by
this plan. Matrix utilities are present in this branch's Geometry Nodes menu.

**Rotate:** use the same selection and transform recipe, including the chosen
pivot. This provides R and the rotation widget without modifying the entire
geometry as Transform Geometry would.

For a captured world-space transform `T` and source object transform `M`, the
replayed local transform is `inverse(M) * T * M`. Translation uses the inverse
linear part on its delta. This matters for rotated, parented and nonuniformly
scaled source objects; simply copying viewport world coordinates into a local
socket is wrong. Initial supported pivots are Median Point, Active Element and
3D Cursor; add Bounding Box Center and Individual Origins with explicit island
tests. Record a fixed pivot/orientation by default; a live procedural pivot is
an optional different behavior.

Reuse native transform input handling where feasible for constraints, numeric
entry and precision. Never replace an EditMesh while a native transform still
owns pointers into it. Keep the interaction snapshot fixed during that modal
step; apply the newly evaluated authoritative result at a safe synchronization
point. Preview and committed geometry must agree before enabling a tool.

**Gate:** point/edge/face moves, connected/disconnected scales and rotations
pass transformed-object and pivot tests. One drag creates one operation; ordinary
cancel/no-op leaves no extra nodes; undo/redo restores both graph and geometry.

Utility sources: [geometry menu](../scripts/startup/bl_ui/node_add_menu_geometry.py#L837),
[Transform Point matrix utility](../source/blender/nodes/function/nodes/node_fn_transform_point.cc#L13).

### Phase 2: extrusion as a topology-and-movement operation

- Route E only for the active authoring session; select the Extrude Mesh domain
  from the active mesh selection domain.
- Face region is the ordinary default: explicitly set Individual=false.
  Individual faces and edge/vertex extrusion receive separate declared modes.
- Create one pending Extrude Mesh operation with the pre-extrusion selection.
  Update Offset/Offset Scale during movement, using captured direction and
  correct local-space conversion. Preview replays from the original input; it
  must not extrude the previous preview again on every mouse event.
- Use Top to select the result for the next interaction, retaining Side as an
  available downstream field. Rebuild the proxy only at a safe operation stage.
- Preserve familiar cancellation: after topology creation, canceling only the
  movement leaves a zero-offset Extrude node/result. Undo removes the whole
  extrusion. A separately labeled Abort Entire Operation can roll back both.
- Resolve and document GN-versus-native back-face/attribute differences through
  fixtures. If exact native region behavior is required, a bounded kernel/mode
  extension is legitimate; do not promise equivalence from the existing node's
  name alone. Manifold extrusion is a later distinct operation.

**Gate:** extrude then move, extrude then scale, and repeated E work in sequence;
Top stays pickable and selected; cancel/undo never leave unintended extra faces.

### Phase 3: loop cut and slide as a two-stage tool

1. Hover a crossing edge and preview its valid quad ring. The hovering edge is
   the input seed, not necessarily the currently selected edge and not an
   Alt-click edge loop. Unsupported poles/non-manifold or intersecting rings
   get a clear visual explanation.
2. Wheel or numeric input changes Cuts on one pending Loop Cut and Slide node;
   Factor stays zero. Abort here removes the whole pending cut.
3. LMB places the cuts and transitions to sliding. Mouse movement updates that
   node's signed Factor; node ring orientation defines the sign, not screen-left.
4. LMB confirms Factor. RMB/Esc in this second stage keeps centered cuts with
   Factor=0, matching the familiar native stage behavior. Undo removes the cut.
5. Use Loops as the active edge selection and expose Loop Index for downstream
   selection or per-loop operations. Rebuild picking against the resulting mesh.

The first version supports the current node's clamped, quad-ring contract.
Native Even/Flipped, smoothing/falloff and unclamped slide are separately scoped
extensions. Factor-only updates may later cache the cut topology, but cache keys
must include input revision, selection and Cuts and stay safe across evaluation.

**Gate:** wheel count, both cancellation stages, factor direction and subsequent
loop transforms pass quad-strip/cylinder/invalid-topology fixtures.

**First release checkpoint:** Phases 0-3 deliver the requested four actions in
their documented scope, plus rotation. Release only after persistence, undo,
input/output routing and repeated mixed-tool chains pass.

### Phase 4: expand with existing-node building blocks

Add each through the same transaction interface:

- Ctrl+B edge bevel and Ctrl+Shift+B vertex bevel, using the current Mesh Bevel
  node with width/segments interaction and explicit offset/control mapping.
- X delete modes and dissolve, using Delete Geometry/Dissolve Elements.
- Merge by Distance with its existing modes; merge to center/cursor/active has
  separate semantics and must not be mislabeled as distance merging.
- Shift+D duplicate-and-move with connected-region rules. Duplicate Elements
  returns duplicates only, so preserve the original via a Join Geometry recipe
  and correctly identify/select the duplicates.
- Selection-compatible split/separate-stream operations; Split Edges is not
  every Y variant, and separate streams do not create a new scene Object.
- Triangulate, flip faces, smooth/flat shading, material assignment and selected
  attribute writes such as sharp/seam/crease, where supported.
- Selection conveniences: box/circle/lasso, linked, grow/shrink, loop/ring,
  boundary, shortest path, similar and saved selections. Make captured membership
  versus live procedural selection visibly distinguishable.

**Gate:** no adapter silently edits only the proxy; inspect and replay its graph
without the session active, and validate attributes/materials/UVs per operation.

### Phase 5: missing selection-aware topology nodes

Recommended order: **Inset -> Edge/Vertex Slide -> Selected Subdivide -> Bridge**.

- Inset needs real thickness/depth and boundary/even-offset rules. Zero-distance
  extrusion plus Scale Elements is not a general constant-width inset.
- Edge/Vertex Slide needs existing-element rails and direction/Even/Clamp rules;
  it is different from creating new loops with Loop Cut and Slide.
- Selected Subdivide needs a selection-aware kernel/node; current Subdivide Mesh
  has no Selection input. Avoid applying it to the entire mesh to imitate a
  selected right-click Subdivide.
- Bridge needs ordered loop inputs, pairing, twist, segmentation and handling of
  unequal/open/closed loops. Add Fill/Grid Fill, connect vertices, poke and edge
  rotation as independently tested projects afterward.
- Bisect is a bounded plane-cut operation that can precede a full K Knife tool.
  Freehand Knife needs viewport picking, stroke storage, cut-through/visibility
  policies and attribute interpolation as well as a geometry kernel.

Where worthwhile, reuse lower-level BMesh algorithms behind Mesh-to-Mesh
kernels, as the existing Loop Cut kernel already does. Do not run context-bound
mesh operators from node evaluation or store only an opaque baked result.

**Gate:** each new kernel has standalone topology/attribute tests and node
evaluation tests before getting an interactive adapter.

### Phase 6: advanced modeling interaction

- Proportional editing with explicit falloff, radius, connected/geodesic and
  projected-distance behavior. A nearest-distance field alone is not every
  native proportional mode.
- Vertex/edge/face/grid snapping. Initially store the resolved transform; live
  target-following constraints require explicit object/geometry dependencies.
- Symmetry and mirror editing, with pairing tolerance and seam/merge rules.
- Shrink/fatten, flatten, to-sphere, smooth/relax, shear and bend, with defined
  pivots and preservation options. Simple variants can use transparent recipes.
- Spin/Screw and manifold extrusion as additional topology operations, not
  merely disconnected duplicate chains.
- UV and attribute authoring where there is a clear persistent data model.
  Sculpt/paint brushes, remesh and multi-object scene operations are separate
  larger projects rather than promised one-node mappings.

**Gate:** replay does not depend on transient tool settings, mouse position,
selection history or another viewport being open.

### Phase 7: broaden the supported contexts and polish

- Nested node groups, shared trees and multiple Viewers/windows require qualified
  context tracking. Offer make-single-user versus edit-shared choices where a
  graph is shared; do not silently change unrelated users.
- Instances need explicit realization or instance-domain editing semantics.
  Curve/point-cloud editing can follow with their own valid operations; mesh
  topology shortcuts should remain mesh-only.
- Simulation/repeat/foreach zones need explicit per-context recording policies;
  a Viewer showing one iteration is not permission to edit only that iteration.
- Improve hover/picking caches, redraw scheduling, node layout and large-selection
  storage. Benchmark dense meshes and repeated topology operations rather than
  promising a fixed frame rate from source inspection.
- Re-edit the latest recorded operation in place through its widgets; a new
  gesture appends a new operation unless the artist explicitly chooses Edit
  Existing Operation. Regeneration must invalidate affected downstream captures.
- Add operation frames, readable labels, bypass/reorder safeguards and optional
  compaction/baking with explicit loss-of-editability warnings.

**Gate:** a supported .blend graph evaluates identically after saving/reloading
without runtime proxy IDs or an active editing session.

## Proposed implementation seams

Use a small authoring interface with real operation adapters rather than separate
ad-hoc insertion code for every shortcut:

| Module | Small interface | Behavior hidden behind it |
|---|---|---|
| Authoring session | Begin, validate, close | Context ownership, proxy IDs, generation checks, lifecycle cleanup |
| Selection snapshot | Capture, resolve, validate | Picked domain, active element, affected points, large index storage, provenance |
| Edit transaction | Begin, update parameters, finish | Reversible graph insertion, undo, operation stage cancellation, layout |
| Operation adapters | Translate a declared operation and parameters | Move/scale/rotate recipes, Extrude and Loop Cut parameter mapping, result selection |
| Preview synchronization | Request revision, accept matching result | Evaluation throttling, stale-result rejection, safe mesh/picking rebuild |

Geometry kernels keep their existing pure node-execution seam. The editor module
authors inputs; it does not become a second geometry implementation that can
disagree with those kernels. Transform and topology adapters are actual varying
callers at the shared transaction seam, so fixes to graph rollback and lifecycle
handling apply to both. Test those interfaces directly as well as the final UI.

Candidate source areas (not files changed by this plan):

- `editors/space_node/node_geometry_viewer_selection.cc` and its declarations.
- `nodes/geometry/nodes/node_geo_viewer.cc` for entry/exit UI and status.
- `editors/transform/` and `editors/space_view3d/` for session-scoped input/widgets.
- `editors/geometry/node_group_operator.cc` as result-storage precedent, not the
  persistent authoring implementation itself.
- `nodes/geometry/nodes/node_geo_select_by_index.cc` or a new captured-selection
  helper for scalable/provenance-aware selection records.
- Existing Set Position/Scale Elements/Extrude/Loop Cut node interfaces and
  `geometry/` kernels; add or extend only where the semantic contract needs it.
- Window-manager lifecycle/undo handling, Blender file read/write/versioning if
  new persistent storage is introduced, and Geometry Nodes evaluation logging.

## Verification and rollout checklist

These are **planned tests**, not tests executed while writing this document.

1. Kernel/recipe tests compare coordinates, connectivity and domain attributes to
   the declared native operation on small fixtures. Include shared vertices,
   disconnected islands, non-manifold data and invalid selections.
2. Transaction tests verify exact link/property restoration, no-op/empty-selection
   rejection, bounded node creation and source-object preservation.
3. UI tests cover G/R/S and widgets, numeric input, constraints, both E/Ctrl+R
   stages, wheel counts, undo/redo and rapid confirmation/cancellation.
4. Chain tests exercise move -> extrude -> scale -> cut -> select new loop ->
   move -> delete, then edit an earlier parameter and verify stale-capture policy.
5. Persistence tests save/reload confirmed graphs, including saving while a
   session is active; runtime selection flags must be reset/reconciled. Run
   headless evaluation to prove independence from viewport context.
6. Lifecycle tests cover frame/upstream changes during a gesture, object/tree/
   Viewer deletion, undo restoring runtime IDs, mode changes, editor/window close,
   multiple viewports and forced evaluation failure.
7. Performance tests measure copying, picking, transform preview and topology
   regeneration at representative mesh sizes. Check older hardware and memory
   use; avoid rebuilding or converting more geometry than the active operation
   requires.

Before implementation, settle the Phase 0 spike decisions: transform-engine
integration, scalable captured-selection storage, provenance validation and
combined graph/proxy undo. These determine the later tools' reliability. No
calendar estimate is reliable until that spike; use the phase gates rather than
invented week-by-week dates.

## Research: existing modeling infrastructure

### Three systems that should not be confused

1. **Mesh Edit Mode** edits an object's `BMEditMesh`/`BMesh`. Native transforms
   create transform data from selected vertices even when the active selection
   domain is Edge or Face. This is why moving a selected face also moves its
   incident vertices, including vertices shared with unselected faces.
   [Mesh transform conversion](../source/blender/editors/transform/transform_convert_mesh.cc#L1490)
   and [selected-vertex rule](../source/blender/editors/transform/transform_convert_mesh.cc#L1512).
2. **Geometry Nodes modifier evaluation** calculates geometry from a graph. Its
   node execution must reproduce an operation from its inputs; it cannot depend
   on whichever viewport is currently focused, transient BMesh selection, or
   today's cursor position. Existing modeling nodes show a concrete route: read a
   selection field and operation parameters, calculate output Mesh, and return
   selection fields for newly created geometry where needed.
   [Extrude Mesh execution](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L1480)
   and [Loop Cut and Slide execution](../source/blender/nodes/geometry/nodes/node_geo_loop_cut_slide.cc#L68).
3. **Node-based Tools** execute a node group against editable object geometry,
   then store the result back into the object. They do not automatically append
   operations to a persistent modifier graph. The current source runs the group
   and calls `store_result_geometry`; Edit Mode results are converted back into
   edit mesh data. This is useful reusable infrastructure, not already the
   requested Viewer authoring mode.
   [Tool execution/store](../source/blender/editors/geometry/node_group_operator.cc#L1036),
   [Edit Mode result storage](../source/blender/editors/geometry/node_group_operator.cc#L669),
   and [official Node-Based Tools manual](https://docs.blender.org/manual/en/latest/modeling/geometry_nodes/tools.html).

Existing **Geometry Nodes gizmos** change exposed input values in the viewport.
They support transform, linear, and dial controls, including propagation through
supported value operations. That makes them a possible input-editing adapter,
but selection, automatic graph insertion, undo, topology changes, and operation
completion still need to be designed. The Transform Gizmo declares controlled
Matrix values, Position, Rotation, and a Transform geometry output; it does not
itself transform selected mesh elements.
[Transform Gizmo declaration](../source/blender/nodes/geometry/nodes/node_geo_gizmo_transform.cc#L16)
and [official gizmos manual](https://docs.blender.org/manual/en/latest/modeling/geometry_nodes/gizmos.html).

### Native interaction infrastructure

- Default transform shortcuts route G/R/S to translate/rotate/resize. Native
  transform machinery already implements modal mouse movement, numeric input,
  constraints, snapping, pivot/orientation selection, proportional editing,
  confirmation, and cancellation. Reuse is plausible, but the ordinary mesh
  adapter writes into Edit Mode vertex coordinates. Connecting those values to
  graph inputs requires an authoring-specific adapter or a controlled proxy
  workflow, not just calling the existing operator and hoping the graph updates.
  [Transform keymap](../scripts/presets/keyconfig/keymap_data/blender_default.py#L384),
  [modal events](../source/blender/editors/transform/transform.cc#L1098),
  [transform properties](../source/blender/editors/transform/transform_ops.cc#L677),
  [gizmo operator binding](../source/blender/editors/transform/transform_gizmo_3d.cc#L1803).
- The mesh transform adapter handles object-to-world matrices and their inverse,
  per-island pivots, symmetry and connected proportional distances. These are
  not incidental details: replaying a transform must preserve its coordinate
  space and pivot convention.
  [Islands and coordinate matrices](../source/blender/editors/transform/transform_convert_mesh.cc#L1551),
  [connected distances](../source/blender/editors/transform/transform_convert_mesh.cc#L1574),
  [symmetry](../source/blender/editors/transform/transform_convert_mesh.cc#L1586).
- A cancelled ordinary transform restores original coordinates. A graph-authoring
  transaction must additionally restore inserted nodes, changed socket values,
  links, and selection/session state.
  [Native transform completion](../source/blender/editors/transform/transform.cc#L2307).

## Research: the four requested interactions

### Move selected points, edges, or faces

The existing Set Position node has Geometry, boolean Selection, Position, and
Offset inputs. It evaluates mesh positions on the Point domain.
[Sockets](../source/blender/nodes/geometry/nodes/node_geo_set_position.cc#L18)
and [mesh execution](../source/blender/nodes/geometry/nodes/node_geo_set_position.cc#L131).

For a normal move, preserve Position's default current-position field and write a
common **local-space Offset**. Writing one absolute Position for a multi-vertex
selection would put all selected vertices at one location. A single-point move
can use an absolute Position if intentionally desired, but a uniform offset
generalizes correctly to all three selection domains. This is a deduction from
the socket/execution semantics, not a new implemented feature.

Selection-domain conversion already exists. The branch's Select by Index field
uses mesh boolean domain adaptation: face-to-point selects vertices incident to
any selected face, and edge-to-point selects the endpoints of selected edges.
There are also explicit reusable mesh-selection helpers.
[Select by Index adaptation](../source/blender/nodes/geometry/nodes/node_geo_select_by_index.cc#L135),
[face-to-point boolean adaptation](../source/blender/blenkernel/intern/mesh_attributes.cc#L207),
[edge-to-point boolean adaptation](../source/blender/blenkernel/intern/mesh_attributes.cc#L478),
[selection helpers](../source/blender/geometry/intern/mesh_selection.cc#L17).

### Scale selected edges or faces

Scale Elements already supports Edge and Face domains, a scalar Scale, Center,
Uniform mode, and Single Axis mode. Connected selected elements form islands;
each island uses mean scale/center values. The exposed scale socket has a
nonnegative minimum. A constant Center can implement a common pivot for a
simple uniform or single-axis transform across disconnected islands.
[Sockets](../source/blender/nodes/geometry/nodes/node_geo_scale_elements.cc#L40),
[island scale calculation](../source/blender/nodes/geometry/nodes/node_geo_scale_elements.cc#L146),
[single-axis calculation](../source/blender/nodes/geometry/nodes/node_geo_scale_elements.cc#L220),
[domains/execution](../source/blender/nodes/geometry/nodes/node_geo_scale_elements.cc#L399),
and [official Scale Elements manual](https://docs.blender.org/manual/en/5.4/modeling/geometry_nodes/mesh/operations/scale_elements.html).

**Parity limits:** this is not a complete implementation of all Edit Mode scaling.
Point-domain scaling, arbitrary nonuniform XYZ factors, custom orientation under
nonuniform object transforms, and exact pivot parity need additional field
construction or a selected-transform operation. The declared nonnegative socket
range is not proof that the kernel cannot calculate a negative scalar; reflection
needs deliberate value representation and parity tests. Do not
silently substitute the node's mean face-center default for Edit Mode's Median
Point: the latter averages selected vertices. Individual Origins groups adjacent
faces/edges rather than detaching them.
[Median Point manual](https://docs.blender.org/manual/en/5.0/editors/3dview/controls/pivot_point/median_point.html),
[Individual Origins manual](https://docs.blender.org/manual/sr/5.2/editors/3dview/controls/pivot_point/individual_origins.html),
and [native island calculation](../source/blender/editors/transform/transform_convert_mesh.cc#L737).

### Extrude with E

Native Edit Mode extrusion is a family of **topology + transform macros**:
region and edge/vertex variants translate; individual faces use shrink/fatten;
region-along-normals and manifold variants have additional semantics.
[Extrusion macros](../source/blender/editors/mesh/mesh_ops.cc#L266).

Existing Extrude Mesh supports vertex, edge, and face modes, boolean Selection,
Offset, Offset Scale, and Individual. `Top` identifies new vertices/new edges/
moved faces depending on mode, while `Side` identifies connecting geometry.
The face socket's default Individual is **true**, so ordinary region extrusion
must explicitly set it to **false**. Region extrusion can use one captured
movement vector; individual extrusion can preserve normal-field displacement.
[Sockets/defaults](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L39),
[mode execution](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L1480),
and [official Extrude Mesh manual](https://docs.blender.org/manual/nb/5.0/modeling/geometry_nodes/mesh/operations/extrude_mesh.html).

**Parity limits:** the official node documentation explicitly warns that its
back-face retention and attribute propagation differ from Edit Mode. A manifold
extrude's orthogonal-edge dissolve and auto-merge are not the same as ordinary
Extrude Mesh. Treat those as separately specified later features.

**Cancellation decision:** native E commits topology before movement; RMB/Esc
in the movement stage leaves zero-offset extruded geometry. A safer all-or-nothing
authoring transaction is possible, but it would deliberately differ from native
behavior and should be documented rather than described as exact parity.
[Extrude Faces manual](https://docs.blender.org/manual/nl/latest/modeling/meshes/editing/face/extrude_faces.html)
and [macro stage separation](../source/blender/editors/mesh/mesh_ops.cc#L266).

### Loop Cut and Slide with Ctrl+R

Native Ctrl+R combines `MESH_OT_loopcut` followed by `TRANSFORM_OT_edge_slide`.
In phase one the hovered **crossing seed edge** determines a quad ring; the wheel
changes cut count. LMB confirms the ring/count, then mouse movement slides the
new loops. RMB before cutting aborts; RMB during sliding leaves centered cuts.
[Macro](../source/blender/editors/mesh/mesh_ops.cc#L224),
[hover picking](../source/blender/editors/mesh/editmesh_loopcut.cc#L352),
[modal confirmation/cancellation](../source/blender/editors/mesh/editmesh_loopcut.cc#L583),
[wheel changes](../source/blender/editors/mesh/editmesh_loopcut.cc#L626),
and [Loop Cut and Slide manual](https://docs.blender.org/manual/en/5.2/modeling/meshes/editing/edge/loopcut_slide.html).

The branch already has `GeometryNodeLoopCutSlide`: Mesh, seed-edge Selection,
Cuts, signed Factor; Mesh, Loops and Loop Index outputs. It is present in the
Geometry Nodes Mesh Operations menu and build source list. Loops is a generated
edge selection; Loop Index is per created loop, with -1 for other edges.
[Sockets](../source/blender/nodes/geometry/nodes/node_geo_loop_cut_slide.cc#L19),
[menu](../scripts/startup/bl_ui/node_add_menu_geometry.py#L558),
[build registration](../source/blender/nodes/geometry/CMakeLists.txt#L198).

**Important correction:** its current kernel is not BMesh-free. Ring discovery
uses native Mesh adjacency, then a temporary BMesh is made, subdivided, adjusted,
and converted back. This is a bounded geometry algorithm, not a persistent Edit
Mode session and not an arbitrary operator replay system.
[Ring discovery](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L73),
[temporary conversion](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L335),
[subdivision](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L373),
[conversion back](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L458).

**Parity limits:** current controls omit native smoothness/falloff, Even, Flipped,
unclamped sliding, and mirror editing. The kernel supports oriented quad rings;
it rejects non-manifold/branching/non-orientable rings and skips perpendicular
ring components sharing a quad. Multiple cuts use
`(cut + 1 + factor) / (cuts + 1)` along each oriented source edge. The interaction
should reuse that signed direction deterministically, rather than assuming
screen-left always means negative Factor.
[Supported ring checks](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L103),
[crossing-ring exclusion](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L155),
[slide formula](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L423),
[native slide controls](../source/blender/editors/transform/transform_ops.cc#L1234).

## Research: additional modeling operation inventory

"Existing" below means the node/kernel is present in this source snapshot, not
that the requested interactive authoring adapter has been implemented or tested.
"Missing" means no dedicated geometry node was found in the inspected node source
inventory/menu; it does not mean the underlying Edit Mode algorithm is absent.

| Interaction | Existing graph building blocks | Gap or caution |
|---|---|---|
| Rotate R; point/vector scale S; mirror | Set Position plus position/rotation/matrix fields | Selected rotation/general transform needs a graph recipe or selected-transform kernel; Transform Geometry acts on the geometry, not a mesh-element Selection. [Set Position](../source/blender/nodes/geometry/nodes/node_geo_set_position.cc#L18). |
| Bevel Ctrl+B / vertex bevel Ctrl+Shift+B | Mesh Bevel already exists, with edge/vertex selection, offsets, Segments, Shape and curve Profile | Interactive width/wheel adapter and parity testing still needed; the current node has four edge endpoint/side offsets rather than the exact native operator controls. [Mesh Bevel sockets](../source/blender/nodes/geometry/nodes/node_geo_mesh_bevel.cc#L32), [kernel interface](../source/blender/geometry/GEO_mesh_bevel.hh#L48). |
| Delete X; dissolve | Delete Geometry and branch Dissolve Elements | Specify each delete mode and selection-domain rule; Dissolve Elements is not every limited/degenerate dissolve option. [Delete](../source/blender/nodes/geometry/nodes/node_geo_delete_geometry.cc#L25), [Dissolve](../source/blender/nodes/geometry/nodes/node_geo_dissolve_elements.cc#L21). |
| Merge by distance | Merge by Distance, including branch All/Connected/Not Connected | Merge-to-center/cursor/active is a separate contract, not the existing distance operation. [Modes](../source/blender/nodes/geometry/nodes/node_geo_merge_by_distance.cc#L18). |
| Inset I | No dedicated Inset Mesh node found | New kernel/node; native region/individual inset exists and supports boundary/even/relative offsets, thickness and depth. [Native inset execution](../source/blender/editors/mesh/editmesh_inset.cc#L245), [options](../source/blender/editors/mesh/editmesh_inset.cc#L601). |
| Bridge edge loops | No dedicated Bridge node found | New kernel/node; explicit two-loop pairing, ordering, twist and interpolation contract needed. [Native bridge](../source/blender/editors/mesh/editmesh_tools.cc#L7470). |
| Edge/vertex slide G,G | No dedicated node found, apart from creating/sliding new cuts | Existing-edge slide needs its own kernel/node or stable generated graph recipe; neighboring rails, Even/Flip/Clamp and UV correction are nontrivial. [Native edge slide](../source/blender/editors/transform/transform_mode_edge_slide.cc#L891), [slide options](../source/blender/editors/transform/transform_ops.cc#L1234). |
| Selected subdivide | Subdivide Mesh and Subdivision Surface exist | Current Subdivide Mesh has Mesh and Level only, no selection; it is not selected-edge Edit Mode subdivide. [Node sockets](../source/blender/nodes/geometry/nodes/node_geo_mesh_subdivide.cc#L15), [native selected subdivide](../source/blender/editors/mesh/editmesh_tools.cc#L90). |
| Duplicate Shift+D | Duplicate Elements, Join Geometry, selection fields | Duplicate Elements outputs only duplicated elements and can duplicate faces separately; preserving a connected selected mesh region like Edit Mode needs explicit topology rules. [Output contract](../source/blender/nodes/geometry/nodes/node_geo_duplicate_elements.cc#L37). |
| Split Y; separate P | Split Edges, Separate Geometry, Split to Instances | Split Edges disconnects face adjacency along edges, not every native Split variant. Separate Geometry separates graph streams; it does not itself create a new Object. [Split Edges](../source/blender/nodes/geometry/nodes/node_geo_edge_split.cc#L16), [Separate Geometry](../source/blender/nodes/geometry/nodes/node_geo_separate_geometry.cc#L21), [Split to Instances](../source/blender/nodes/geometry/nodes/node_geo_split_to_instances.cc#L27). |
| Spin / Screw | Can compose repeat/extrude/transform recipes; no dedicated node found | New topology kernel/node if exact connected Edit Mode result is required. [Native Spin](../source/blender/editors/mesh/editmesh_extrude_spin.cc#L67), [native Screw](../source/blender/editors/mesh/editmesh_extrude_screw.cc#L119). |
| Triangulate; smooth/flat shading; material assignment | Triangulate, Set Shade Smooth, Set Material/Material Index | Good lower-risk interaction adapters once selection capture is reliable. [Triangulate selection](../source/blender/nodes/geometry/nodes/node_geo_triangulate.cc#L63), [shade selection](../source/blender/nodes/geometry/nodes/node_geo_set_shade_smooth.cc#L25). |
| Flip / custom normals / sharp edges | Flip Faces, Set Mesh Normal, Set Shade Smooth, attribute nodes | Orientation recalculation is not identical to setting shading normals; mark-sharp/seam/crease needs well-defined selected attribute writes. [Normal modes](../source/blender/nodes/geometry/nodes/node_geo_set_mesh_normal.cc#L31). |
| Smooth vertices; shrink/fatten; flatten; to-sphere | Position/Normal, Blur Attribute, Set Position can express simple variants | Native boundary/volume/corrective options and connected-island pivots require explicit parity targets. [Blur Attribute inputs](../source/blender/nodes/geometry/nodes/node_geo_blur_attribute.cc#L41). |
| Knife / bisect / fill / grid fill / poke / connect vertices / edge rotate | Edit Mode algorithms exist; no direct one-to-one node inventory matches found | Separate topology-node projects; do not silently bake an opaque modified Mesh to stand in for an editable operation graph. [Native operator registration](../source/blender/editors/mesh/mesh_ops.cc#L159). |
| Selection grow/shrink, linked, ring/loop, boundary, shortest path, similar | Native selection tools and existing Loop Selection/mesh topology fields | Selection result can be captured as indices for immediate replay or represented by a procedural field when intended; those behaviors must be visibly distinct. [Native selections](../source/blender/editors/mesh/mesh_ops.cc#L116). |
| Proportional editing, snapping, symmetry | Native transform infrastructure already exists | They affect the operation result. Replay needs persistent falloff/selection/space or captured displacement fields, not access to transient viewport settings. [Proportional/snap properties](../source/blender/editors/transform/transform_ops.cc#L689), [connected distance implementation](../source/blender/editors/transform/transform_convert_mesh.cc#L1005). |

## Research: replay and safety constraints

These are design deductions from the source above, not implemented guarantees.

- **Indices are locations in one evaluated topology, not persistent element IDs.**
  Select by Index rebuilds a mask over the mesh's current domain size. Topology
  nodes can reorder or replace elements; both extrusion and loop-cut kernels
  explicitly randomize order in applicable debug builds. Equal element counts
  do not prove equal connectivity or ordering. Snapshot selection must belong
  to the exact input of the newly authored node and be guarded by a topology/
  provenance check when upstream topology later changes.
  [Index field](../source/blender/nodes/geometry/nodes/node_geo_select_by_index.cc#L135),
  [extrusion randomization](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L1528),
  [loop-cut randomization](../source/blender/geometry/intern/mesh_loop_cut_slide.cc#L463).
  The debug permutation is deterministic for a fixed seed, using mesh vertex
  count; this is not evidence that unchanged evaluation randomly reorders the
  mesh on every frame. It does reinforce that index order is an implementation
  detail rather than a permanent identity contract.
  [Deterministic permutation and seed](../source/blender/geometry/intern/randomize.cc#L29).
- **A generic `id` attribute is not enough.** Extrusion copies point attributes,
  so source IDs can be duplicated. A robust later identity scheme needs lineage
  and per-generated-element identity rather than treating all inherited IDs as
  globally unique. Existing `Top`, `Side`, `Loops`, and Loop Index outputs are
  useful immediate result selectors, but do not make arbitrary downstream
  topology changes identity-safe.
  [Attribute copying](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L1368),
  [generated extrusion selections](../source/blender/nodes/geometry/nodes/node_geo_extrude_mesh.cc#L1495),
  [generated loop selections](../source/blender/nodes/geometry/nodes/node_geo_loop_cut_slide.cc#L41).
- **Keep execution pure and authoring contextual.** A temporary BMesh inside a
  Mesh-to-Mesh kernel is compatible with reevaluation. Running a context-bound
  Edit Mode operator from modifier evaluation is a different architecture and
  must not be assumed safe. Node operations must retain needed attributes,
  materials, UV seams/corners and unsupported-geometry behavior across conversion.
- **Preview must not accumulate edits.** Each modal parameter update should
  replay one operation from its frozen input snapshot, not apply another
  extrusion/cut to the preceding preview. The native inset operator already
  illustrates restoring a saved mesh before rerunning a modal topology update.
  [Inset redo snapshot/restore](../source/blender/editors/mesh/editmesh_inset.cc#L176)
  and [rerun from snapshot](../source/blender/editors/mesh/editmesh_inset.cc#L264).
- **Preserve the active graph path.** Authoring from the selected Viewer should
  insert only on the intended input link. Changing shared upstream output links,
  Group Output, other Viewers, nested group instances, or simulation/repeat-zone
  execution is a separate scope decision; source-input identity and evaluation
  context must be retained.
- **Snapping result versus snapping rule are different products.** Recording the
  resolved offset gives deterministic replay; recreating a live target-dependent
  snapping relationship requires graph dependencies and target controls.
- **Connectivity changes are session events.** After confirmed extrude/cut/delete,
  rebuild picking and selection against the node's new evaluated output, and
  use generated output selections where available. Continuing to pick from an
  older Edit Mode proxy would author indices against the wrong topology.
- **Reevaluation conflicts need an explicit policy.** Frame changes, upstream
  edits, undo/redo, object/mode changes, file loading, and asynchronous evaluation
  can invalidate a modal operation's snapshot. Cancel or rebase with a visible
  warning; never quietly retarget an operation to a different mesh.

The proposal above uses these findings to define phases, implementation seams,
release scope and verification gates. Only this planning document was created;
no feature implementation, build or runtime validation was performed for this
request.
