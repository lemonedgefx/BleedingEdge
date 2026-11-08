# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Phase 1 native Viewer transform adapter. Requires a build containing Phase 1.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
The separate recipes test can run on an older build; this one deliberately cannot.
"""

import bmesh
import bpy
from mathutils import Matrix, Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = 'Viewer Transform Test'
TREE = 'Viewer Transform Test'


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def source():
    return bpy.data.objects[SOURCE]


def tree():
    return bpy.data.node_groups[TREE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer()
                and l.to_socket.type == 'GEOMETRY')


def evaluate():
    bpy.context.view_layer.update()
    obj = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = obj.to_mesh()
    positions = [v.co.copy() for v in mesh.vertices]
    obj.to_mesh_clear()
    return positions


def result_positions():
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
    operation = result_node()
    tree().links.new(next(s for s in operation.outputs if s.type == 'GEOMETRY'), output.inputs['Geometry'])
    try:
        return evaluate()
    finally:
        tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        evaluate()


def close(actual, expected):
    assert len(actual) == len(expected)
    for i, (a, b) in enumerate(zip(actual, expected)):
        assert (a - b).length < 1e-4, (i, a, b)


def refresh():
    # A background run has no event loop; use the same fresh-evaluation/validation seam.
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
    evaluate()
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def finish():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    assert bpy.context.mode == 'OBJECT'
    assert bpy.context.object == source()
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def select(domain):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    collection = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    picked = [0, 1] if domain == 'VERT' else [0]
    for index in picked:
        collection[index].select_set(True)
        bm.select_history.add(collection[index])
    if domain == 'FACE':
        bm.faces.active = collection[0]
    bm.select_flush_mode()
    points = [v.index for v in bm.verts if v.select]
    pivot = sum((bm.verts[i].co for i in points), Vector()) / len(points)
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
    return points, pivot


bpy.ops.mesh.primitive_cube_add()
source_object = bpy.context.object
source_object.name = SOURCE
source_object.rotation_euler = (0.2, -0.4, 0.3)
source_object.scale = (1.5, 0.7, 2)
parent = bpy.data.objects.new('Viewer Transform Parent', None)
bpy.context.collection.objects.link(parent)
parent.rotation_euler = (-0.3, 0.2, 0.1)
parent.scale = (2, 0.5, 1.25)
source_object.parent = parent  # Deliberately includes inherited shear.
node_tree = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
node_tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
node_tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = node_tree.nodes.new('NodeGroupInput')
output_node = node_tree.nodes.new('NodeGroupOutput')
viewer_node = node_tree.nodes.new('GeometryNodeViewer')
viewer_node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
node_tree.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
node_tree.links.new(input_node.outputs['Geometry'], viewer_node.inputs['Geometry'])
modifier = source_object.modifiers.new('Viewer Transform Test', 'NODES')
modifier.node_group = node_tree
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
base = evaluate()
world = source_object.matrix_world.copy()
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer transform baseline')

cases = 0
for domain in ('VERT', 'EDGE', 'FACE'):
    for kind in ('MOVE', 'SCALE', 'ROTATE'):
        begin()
        points, median = select(domain)
        before = result_positions()
        pivot = world @ median
        bpy.context.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
        count = len(tree().nodes)
        with bpy.context.temp_override(**override(VIEW_AREA)):
            common = dict(use_proportional_edit=False, mirror=False)
            if kind == 'MOVE':
                delta = Vector((0.3, -0.2, 0.4))
                assert bpy.ops.transform.translate(value=delta, orient_type='GLOBAL', **common) == {'FINISHED'}
                transform = Matrix.Translation(delta)
            elif kind == 'SCALE':
                assert bpy.ops.transform.resize(value=(1.7,) * 3, orient_type='GLOBAL', **common) == {'FINISHED'}
                transform = Matrix.Translation(pivot) @ Matrix.Diagonal((1.7, 1.7, 1.7, 1)) @ Matrix.Translation(-pivot)
            else:
                assert bpy.ops.transform.rotate(value=0.63, orient_type='GLOBAL', orient_axis='Z',
                                                constraint_axis=(False, False, True), **common) == {'FINISHED'}
                transform = Matrix.Translation(pivot) @ Matrix.Rotation(0.63, 4, 'Z') @ Matrix.Translation(-pivot)
        local = world.inverted() @ transform @ world
        expected = [local @ p if i in points else p.copy() for i, p in enumerate(before)]
        operation = result_node()
        scale_elements = kind == 'SCALE' and domain != 'VERT'
        expected_type = ('GeometryNodeGroup' if kind == 'ROTATE' else
                         'GeometryNodeScaleElements' if scale_elements else 'GeometryNodeSetPosition')
        assert operation.bl_idname == expected_type
        added = 1 if kind == 'ROTATE' else 2 if scale_elements or kind == 'MOVE' else 7
        assert len(tree().nodes) == count + added
        if kind == 'ROTATE':
            assert operation.label == 'Set Rotation' and operation.node_tree.name.startswith('Set Rotation')
            assert [s.name for s in operation.inputs] == ['Mesh', 'Rotation']
            assert [s.name for s in operation.outputs] == ['Mesh']
            assert abs(operation.inputs['Rotation'].default_value[2] - 0.63) < 1e-5
            capture = next(n for n in operation.node_tree.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
        else:
            capture = next(l.from_node for l in tree().links if l.to_node == operation and l.to_socket.name == 'Selection')
        assert capture.bl_idname == 'GeometryNodeSelectByIndex' and capture.mode == 'CAPTURED'
        assert capture.domain == {'VERT': 'POINT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain]
        if scale_elements:
            assert operation.inputs['Center'].is_linked
            assert operation.domain == capture.domain
        close(result_positions(), expected)
        close(evaluate(), base)  # Group Output and original source still unchanged.
        close([v.co for v in source().data.vertices], base)
        refresh()
        assert bpy.context.mode == 'EDIT_MESH'
        assert len(tree().nodes) == count + added
        current = bmesh.from_edit_mesh(bpy.context.object.data)
        current.verts.index_update()
        close([v.co for v in current.verts], expected)
        assert [v.index for v in current.verts if v.select] == points
        del current
        # A second gesture operates on the first result, not on its old input snapshot.
        with bpy.context.temp_override(**override(VIEW_AREA)):
            delta = Vector((0, 0, 0.15))
            assert bpy.ops.transform.translate(value=delta, orient_type='GLOBAL', **common) == {'FINISHED'}
        offset = world.inverted().to_3x3() @ delta
        expected = [p + offset if i in points else p for i, p in enumerate(expected)]
        close(result_positions(), expected)
        refresh()
        finish()
        cases += 1

# Representable local-axis scale uses Scale Elements. A world-axis scale under inherited shear
# must use the general matrix recipe instead of silently approximating its scalar controls.
source().parent = None
source().scale = (1, 1, 1)
bpy.context.view_layer.update()
begin()
select('FACE')
count = len(tree().nodes)
bpy.context.scene.tool_settings.transform_pivot_point = 'ACTIVE_ELEMENT'
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.5, 1, 1), orient_type='LOCAL',
                                    constraint_axis=(True, False, False),
                                    use_proportional_edit=False, mirror=False) == {'FINISHED'}
assert result_node().bl_idname == 'GeometryNodeScaleElements'
assert result_node().inputs['Scale Mode'].default_value == 'Single Axis'
assert len(tree().nodes) == count + 2
refresh()
finish()
source().parent = parent
source().scale = (1.5, 0.7, 2)
bpy.context.view_layer.update()
begin()
select('FACE')
count = len(tree().nodes)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.5, 1, 1), orient_type='GLOBAL',
                                    constraint_axis=(True, False, False),
                                    use_proportional_edit=False, mirror=False) == {'FINISHED'}
assert result_node().bl_idname == 'GeometryNodeSetPosition'
assert len(tree().nodes) == count + 7
refresh()
finish()

# Native no-op/rejected operations must not produce nodes or an undo step.
begin()
select('FACE')
bpy.context.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
count = len(tree().nodes)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=(0, 0, 0)) == {'CANCELLED'}
    assert bpy.ops.transform.resize(value=(1, 1, 1)) == {'CANCELLED'}
    assert bpy.ops.transform.rotate(value=0) == {'CANCELLED'}
    assert bpy.ops.transform.transform(mode='SHEAR', value=(0.3, 0, 0, 0)) == {'CANCELLED'}
    assert not bpy.ops.mesh.extrude_region_move.poll()
assert len(tree().nodes) == count
assert bpy.context.mode == 'EDIT_MESH'
finish()

# One native confirmation = one global graph step, without resurrection of runtime proxies.
begin()
select('FACE')
count = len(tree().nodes)
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Before one Viewer gesture')
# That unrelated explicit push ends editing safely; begin again without another undo push.
if bpy.context.mode != 'EDIT_MESH':
    begin()
    select('FACE')
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=(0.2, 0, 0), orient_type='GLOBAL') == {'FINISHED'}
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Scripted Viewer transform')
refresh()
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
assert len(tree().nodes) == count
assert bpy.context.mode == 'OBJECT'
assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.ed.redo() == {'FINISHED'}
assert len(tree().nodes) == count + 2
assert bpy.context.mode == 'OBJECT'
print('GEOMETRY_VIEWER_TRANSFORM_SMOKE_OK', cases, flush=True)
