# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Viewer loop-cut adapter, continuation, undo and persistence. Requires Phase 3.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
Do not run on the older Phase 0 executable; use the separate recipes test there.
"""

import os
import tempfile

import bmesh
import bpy
from mathutils import Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Loop Cut Test'


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def tree():
    return bpy.data.node_groups[TREE]


def source():
    return bpy.data.objects[SOURCE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer()
                and l.to_socket.type == 'GEOMETRY')


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def refresh():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}


def evaluate_branch(loops=False):
    operation = result_node()
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
    geometry = next(s for s in operation.outputs if s.type == 'GEOMETRY')
    store = None
    if loops:
        store = tree().nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type, store.domain = 'BOOLEAN', 'EDGE'
        store.inputs['Name'].default_value = '.test_loops'
        tree().links.new(geometry, store.inputs['Geometry'])
        tree().links.new(operation.outputs['Loops'], store.inputs['Value'])
        geometry = store.outputs['Geometry']
    tree().links.new(geometry, output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        evaluated = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh(preserve_all_data_layers=True,
                                 depsgraph=bpy.context.evaluated_depsgraph_get())
        result = ([v.co.copy() for v in mesh.vertices], [tuple(e.vertices) for e in mesh.edges],
                  [tuple(f.vertices) for f in mesh.polygons])
        selected = [i for i, v in enumerate(mesh.attributes['.test_loops'].data) if v.value] if loops else []
        assert not any(a.name.startswith('.viewer_cut_loops_') for a in mesh.attributes)
        evaluated.to_mesh_clear()
        return result, selected
    finally:
        tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        if store:
            tree().nodes.remove(store)
        bpy.context.view_layer.update()


def assert_proxy(expected, selected):
    assert bpy.context.mode == 'EDIT_MESH'
    assert bpy.context.tool_settings.mesh_select_mode[:] == (False, True, False)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    assert tuple(map(len, (bm.verts, bm.edges, bm.faces))) == tuple(map(len, expected))
    assert all((v.co - co).length < 1e-4 for v, co in zip(bm.verts, expected[0]))
    assert [tuple((e.verts[0].index, e.verts[1].index)) for e in bm.edges] == expected[1]
    assert [e.index for e in bm.edges if e.select] == selected


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)


mesh = bpy.data.meshes.new(SOURCE)
mesh.from_pydata([(x, y, 0) for y in range(2) for x in range(4)], [],
                 [(x, x + 1, x + 5, x + 4) for x in range(3)])
obj = bpy.data.objects.new(SOURCE, mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
obj.rotation_euler = (0.2, -0.4, 0.3)
obj.scale = (1.5, 0.7, 2)
parent = bpy.data.objects.new('Loop Cut Sheared Parent', None)
bpy.context.collection.objects.link(parent)
parent.rotation_euler = (-0.3, 0.2, 0.1)
parent.scale = (2, 0.5, 1.25)
obj.parent = parent
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = group.nodes.new('NodeGroupInput')
output_node = group.nodes.new('NodeGroupOutput')
node = group.nodes.new('GeometryNodeViewer')
node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
original = [v.co.copy() for v in mesh.vertices]
seed = next(e.index for e in mesh.edges if set(e.vertices) == {0, 4})
obj = mesh = group = parent = node = input_node = output_node = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Loop Cut Baseline')
begin()

# The hovered/explicit seed is independent of both selected elements and the selection domain.
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_loop_cut.poll()
    assert not bpy.ops.mesh.loopcut_slide.poll()
    bpy.ops.mesh.select_mode(type='FACE')
    bpy.ops.mesh.select_all(action='DESELECT')
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=-1) == {'CANCELLED'}
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=10**6) == {'CANCELLED'}
assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
bm = bmesh.from_edit_mesh(bpy.context.object.data)
bm.faces.ensure_lookup_table()
bm.faces[0].select_set(True)
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.hide(unselected=False)
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=seed) == {'CANCELLED'}
    bpy.ops.mesh.reveal(select=False)
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=seed, cuts=2, factor=0.35) == {'FINISHED'}
assert_clean(5)  # Only the capture and ordinary loop-cut node remain.
assert result_node().bl_idname == 'GeometryNodeLoopCutSlide'
assert result_node().inputs['Cuts'].default_value == 2
assert abs(result_node().inputs['Factor'].default_value - 0.35) < 1e-5
assert {'Loops', 'Loop Index'} <= {s.name for s in result_node().outputs}
expected, selected = evaluate_branch(loops=True)
assert len(selected) == 6  # Two cuts through the three-quad strip.
refresh()
assert_proxy(expected, selected)

# The authoritative Loops selection is usable by subsequent transform adapters.
affected = {v for i in selected for v in expected[1][i]}
delta = Vector((0.2, -0.1, 0.3))
local_delta = source().matrix_world.inverted().to_3x3() @ delta
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=delta, use_proportional_edit=False, mirror=False) == {'FINISHED'}
assert_clean(7)
moved, _ = evaluate_branch()
for i, (actual, previous) in enumerate(zip(moved[0], expected[0])):
    assert (actual - previous - (local_delta if i in affected else Vector())).length < 1e-4
refresh()
assert_proxy(moved, selected)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.2,) * 3, use_proportional_edit=False, mirror=False) == {'FINISHED'}
scaled, _ = evaluate_branch()
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Before scripted repeated cut')
refresh()
assert_proxy(scaled, selected)
assert len(tree().nodes) == 9  # Select by Index supplies the dynamic pivot to Scale Elements.

# Each stage captures its own input; repeated cut followed by E uses the new edge selection.
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=seed, cuts=1) == {'FINISHED'}
assert_clean(11)
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Scripted repeated cut')
next_result, next_loops = evaluate_branch(loops=True)
refresh()
assert_proxy(next_result, next_loops)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_extrude(offset=(0, 0, 0.25)) == {'FINISHED'}
assert_clean(14)
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Scripted extrusion after cut')
refresh()
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
assert_clean(11)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
assert_clean(9)  # One step removes the repeated cut, including its capture.
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.redo() == {'FINISHED'}
assert_clean(11)
assert all((v.co - p).length < 1e-6 for v, p in zip(source().data.vertices, original))
output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
assert next(l.from_node.bl_idname for l in tree().links if l.to_node == output) == 'NodeGroupInput'

expected, selected = evaluate_branch(loops=True)
with tempfile.TemporaryDirectory(prefix='viewer-loop-cut-') as directory:
    path = os.path.join(directory, 'loop-cut.blend')
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.wm.save_as_mainfile(filepath=path)
    # File loading replaces editor data; do not restore a pre-load area override afterward.
    bpy.ops.wm.open_mainfile(filepath=path)
    assert len(tree().nodes) == 11
    replayed, replayed_loops = evaluate_branch(loops=True)
    assert replayed[1:] == expected[1:] and replayed_loops == selected
    assert all((a - b).length < 1e-5 for a, b in zip(replayed[0], expected[0]))
print('GEOMETRY_VIEWER_LOOP_CUT_SMOKE_OK')
