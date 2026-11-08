# Geometry Nodes curve editing: deferred roadmap

Recorded: 2026-10-13.

**Status: future proposal, explicitly set aside.** Saving this roadmap does not
authorize implementation, tests that mutate projects, or a build. Resume only
when the user explicitly requests curve editing. This is separate from the
[active mesh-editing roadmap](geometry-nodes-interactive-edit-roadmap.md).

## Goal and initial scope

Extend Viewer Edit Mode so that viewport curve editing authors an editable
Geometry Nodes chain between the captured input stage and Viewer. Do not bake
an opaque edited curve or modify the source object. Reuse the shared transaction,
node-layout, confirm/cancel, undo and lifecycle infrastructure, but use a
curve-specific editing proxy and selection model rather than a mesh BMesh.

Start with **Poly and Bezier splines** in one realized Curve geometry component,
in the root tree of one modifier. Reject mixed components and unresolved
instances initially. Catmull-Rom, NURBS-specific editing, Grease Pencil, nested
groups, multiple viewers and multi-object editing are later work.

The current Viewer editing adapter and Select Elements field/capture evaluation
are mesh-only. Existing native Curves tools and curve Geometry Nodes are reuse
opportunities, not evidence that these Viewer adapters already exist.

## Phase 0: curve editing foundation

- Enable Viewer Edit Mode for a Curve component and create a temporary curve
  editing object using Blender's modern Curves data representation.
- Display the evaluated smooth curve, control points, control polygon and Bezier
  handles. Select control points, not tessellated drawing samples.
- Support control-point and whole-spline selection. Keep left/right handle
  masks separately; handles are point attributes, not independent domains.
- Extend the existing Select Elements node's Manual and Captured modes with
  Curve point/spline support and structured handle selection. Preserve all
  existing mesh projects and captures; do not create a second capture node.
- Adapt click, box, circle, lasso, linked, endpoint and grow/shrink selection.
- Preserve active selection, source context, local hiding, node layout and
  undo/redo. Import rejects empty or invalid selections.
- Validate ordered spline offsets, spline type, cyclic state and element
  identity. Allow position-only replay where identity can be established;
  reject unexpected reordering or ambiguous captures. Active gestures also
  validate control/handle positions and the exact editing context.

**Gate:** picking and import, empty guards, deformation/reordering checks,
copy/save/reload/undo, source-context restoration and curve drawing pass without
changing the source geometry or creating durable proxy IDs.

## Phase 1: basic transforms

- G/move widget: move selected control points.
- R/rotate widget: rotate selected points or complete splines.
- S/scale widget: scale selected points or complete splines.
- Reuse axis constraints, numeric input, precision and supported transform pivots.
- Author captured selections plus Set Position/matrix-field recipes, then resume
  on the evaluated result after confirmation and one graph undo step.

Poly curves can support the full affine transform recipe first. Bezier
control-point translation must carry its handles. Full Bezier rotation/scaling
requires Phase 2 handle-aware authoring; moving control points alone is not
equivalent to transforming the whole Bezier shape.

**Gate:** native-transform parity within the supported scope, including rotated,
nonuniformly scaled and parent-sheared source objects, confirm/cancel, pivots and
repeated transform chains.

## Phase 2: Bezier handles and shape controls

- Move, rotate and scale individual handles or both handles.
- Set Automatic, Aligned, Vector or Free handle types.
- Preserve alignment, and record handle-type changes caused by manual movement.
- Complete handle-aware Bezier control-point and whole-spline rotation/scaling.
- Adjust or reset selected-point radius and tilt.
- Open/close selected splines, reverse direction and change resolution.

Reuse Set Handle Positions, Set Handle Type, Set Curve Radius, Set Curve Tilt,
Set Spline Cyclic, Reverse Curve and Set Spline Resolution where their semantics
match. Recalculate automatic handles appropriately; do not translate and then
transform a handle twice, or force automatic/vector handles to free silently.

**First useful release checkpoint:** Phases 0-2 provide direct curve shaping
without changing control-point counts. Cyclic state and representation changes
still require explicit result validation and renewed captures.

## Phase 3: essential topology editing

- E: extrude selected endpoints, then move the new points.
- Subdivide selected segments with an adjustable cut count.
- X/Delete: remove selected control points or complete splines.
- Shift+D: duplicate complete splines, retaining the original, and move copies.
- Select actual newly created points/splines for the next gesture.

Endpoint extrusion needs a procedural curve node and a shared pure geometry
kernel extracted/adapted from native Curves editing. Endpoint-only behavior is
the initial contract; interior ranges and closed curves need separate rules.
Never call context-bound editor operators from node evaluation.

Subdivide Curve already has a Cuts field for the segment following each control
point, not a Selection socket. Build an explicit segment mask, including the
last-to-first segment of cyclic splines, and add reliable generated-element
mapping/outputs as necessary. Do not subdivide an entire spline when only a few
segments were picked.

Whole-spline deletion/duplication can use existing geometry recipes. Point
removal that reconnects surviving neighbors and deletion that breaks a curve
into pieces must be distinct operations. Result masks must come from evaluated
topology or authoritative lineage, not pre-edit raw indices.

**Gate:** open/closed splines, repeated editing, minimum point counts, radius,
tilt, handles, material/custom attributes, cancellation stages and one-step
undo/redo pass with replay independent of the editing session.

## Phase 4: connectivity and refinement

- Split at selected points or segments.
- Connect two compatible endpoints; explicitly distinguish connecting splines
  from closing one spline.
- Weld compatible endpoints or adjacent points with declared tolerance and
  attribute/handle rules. Do not create unsupported branching spline topology.
- Duplicate selected contiguous sections rather than reconnecting disjoint picks.
- Separate sections into geometry streams, without creating scene objects.
- Interactive Trim Curve, Resample Curve and selected-corner Fillet Curve.
- Convert supported spline types.

Trim, Resample and Fillet have existing nodes/kernels. Connect, weld, partial
duplication and split need additional procedural topology support or verified
recipes. Resampling/conversion must expose when representation, control-point
identity or Bezier handles change. Zero radius/unselected cuts fields must not
silently rewrite untouched regions; test that contract before adopting a recipe.

**Gate:** attributes and handle semantics survive supported operations, selections
are mapped explicitly, incompatible endpoints are rejected and saved graphs
replay without a viewport.

## Phase 5: draw and insert

- Add curve primitives to the current Viewer branch.
- Click to draw a Poly spline; click-drag to draw Bezier points/handles.
- Extend an existing endpoint.
- Insert a point on a hovered segment at a chosen parameter, preserving shape
  where supported. This is not merely an evenly spaced Subdivide operation.
- Freehand drawing with adjustable curve-fitting tolerance.

Store strokes/control-point creation data in editable procedural inputs or
nodes. Define drawing plane, depth/surface picking, confirm/cancel and attribute
initialization before implementation. Preserve the native curve type when
possible rather than always converting drawn data into resampled Poly curves.

## Phase 6: advanced editing and broader support

- Proportional editing along a spline, with explicit falloff and connected scope.
- Grid, point and surface snapping, with captured versus live-target semantics.
- Smooth/relax with endpoint and shape-preservation controls.
- Symmetry and mirror editing.
- Catmull-Rom and NURBS-specific editing, including weights, order and knots.
- Re-edit the latest recorded operation through viewport widgets.
- Broaden qualified modifier/group/instance/multi-viewer contexts after the
  single-component foundation is reliable. Grease Pencil remains separate work.

## Recommended sequence and verification

**Foundation -> transforms -> handles -> endpoint extrusion/subdivision ->
connectivity -> drawing -> advanced editing.**

Every phase must verify selection continuity, open/closed and multiple splines,
mixed supported types, control-point/handle behavior, radius/tilt/material/custom
attributes, source object transforms, graph layout, undo and save/reload. Keep
mesh Viewer regressions running when shared authoring infrastructure changes.
Implementation and build status must be recorded separately from this proposal.

## Source references from the planning inspection

- [Viewer mesh-only scope](../source/blender/editors/space_node/node_geometry_viewer_selection.cc)
- [Select Elements mesh field/capture](../source/blender/nodes/geometry/nodes/node_geo_select_by_index.cc)
- [Modern Curves editor operations](../source/blender/editors/curves/intern/curves_ops.cc)
- [Native Curves extrusion](../source/blender/editors/curves/intern/curves_extrude.cc)
- [Control-point movement and handle translation](../source/blender/nodes/geometry/nodes/node_geo_set_position.cc)
- [Handle-position/type behavior](../source/blender/nodes/geometry/nodes/node_geo_set_curve_handles.cc)
- [Set Handle Type](../source/blender/nodes/geometry/nodes/node_geo_curve_set_handle_type.cc)
- [Subdivide Curve Cuts field](../source/blender/nodes/geometry/nodes/node_geo_curve_subdivide.cc)
- [Native selected-segment subdivision](../source/blender/editors/curves/intern/curves_ops.cc)
- [Remove points and split kernel](../source/blender/geometry/GEO_curves_remove_and_split.hh)
- [Endpoint merge kernel](../source/blender/geometry/GEO_merge_curves.hh)
- [Curve smoothing kernel](../source/blender/geometry/GEO_smooth_curves.hh)
- [Modern Curves pen interaction](../source/blender/editors/curves/intern/curves_pen.cc)
