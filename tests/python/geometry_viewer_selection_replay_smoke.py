# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A real Viewer E -> S -> changed extrusion Offset regression.

Run with --background --factory-startup --python-exit-code 1 --python this_file.
An optional -- --legacy-fixture PATH writes the baseline with the old build.
"""

import argparse
import sys

import bmesh
import bpy
from mathutils import Vector


parser = argparse.ArgumentParser()
parser.add_argument('--legacy-fixture')
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def refresh():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}


bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
obj.name = 'Selection Replay Source'
group = bpy.data.node_groups.new('Selection Replay Tree', 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
source = group.nodes.new('NodeGroupInput')
output = group.nodes.new('NodeGroupOutput')
viewer = group.nodes.new('GeometryNodeViewer')
viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(source.outputs['Geometry'], output.inputs['Geometry'])
group.links.new(source.outputs['Geometry'], viewer.inputs['Geometry'])
obj.modifiers.new('Selection Replay', 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
group.nodes.active = viewer
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.node.activate_viewer()
    bpy.context.view_layer.update()
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.mesh.select_mode(type='FACE')
bm = bmesh.from_edit_mesh(bpy.context.object.data)
bm.faces.ensure_lookup_table()
face = bm.faces[0]
face.select_set(True)
bm.faces.active = face
bm.select_history.add(face)
bm.select_flush_mode()
bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
bm = face = None
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_extrude(offset=(0, 0, 1)) == {'FINISHED'}
refresh()
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.5,) * 3, use_proportional_edit=False,
                                    mirror=False) == {'FINISHED'}
refresh()
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
scale = next(n for n in group.nodes if n.bl_idname == 'GeometryNodeScaleElements')
extrude = next(n for n in group.nodes if n.bl_idname == 'GeometryNodeExtrudeMesh')
capture = next(l.from_node for l in group.links if l.to_node == scale and l.to_socket.name == 'Selection')
offset = next(l.from_node for l in group.links if l.to_node == extrude and l.to_socket.name == 'Offset')
assert capture.selected_count == 1 and capture.domain == 'FACE'


def evaluate(geometry, fields=()):
    stores = []
    socket = geometry
    for name, field in fields:
        store = group.nodes.new('GeometryNodeStoreNamedAttribute')
        stores.append(store)
        store.data_type = 'BOOLEAN'
        store.domain = 'POINT'
        store.inputs['Name'].default_value = name
        group.links.new(socket, store.inputs['Geometry'])
        group.links.new(field, store.inputs['Value'])
        socket = store.outputs['Geometry']
    group.links.new(socket, output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
        result = ([v.co.copy() for v in mesh.vertices],
                  [tuple(e.vertices) for e in mesh.edges],
                  [tuple(f.vertices) for f in mesh.polygons])
        attributes = {name: [v.value for v in mesh.attributes[name].data] for name, _ in fields}
        evaluated.to_mesh_clear()
        return result, attributes
    finally:
        group.links.new(source.outputs['Geometry'], output.inputs['Geometry'])
        for store in stores:
            group.nodes.remove(store)


stage_before, masks_before = evaluate(extrude.outputs['Mesh'],
                                    [('replay_points', capture.outputs['Points']),
                                     ('replay_valid', capture.outputs['Valid'])])
assert all(masks_before['replay_valid'])
selected = {i for i, value in enumerate(masks_before['replay_points']) if value}
assert len(selected) == 4, selected
before, _ = evaluate(scale.outputs['Geometry'])
if args.legacy_fixture:
    manual = group.nodes.new('GeometryNodeSelectByIndex')
    manual.name = 'Legacy Manual Selection'
    manual.domain = 'EDGE'
    manual.inputs['Indices'].default_value = '0, 2-4'
    bpy.ops.wm.save_as_mainfile(filepath=args.legacy_fixture)
delta = Vector((0.5, 0.25, 1))
offset.vector = Vector(offset.vector) + delta
stage_after, masks_after = evaluate(extrude.outputs['Mesh'],
                                  [('replay_points', capture.outputs['Points']),
                                   ('replay_valid', capture.outputs['Valid'])])
after, _ = evaluate(scale.outputs['Geometry'])
assert stage_before[1:] == stage_after[1:], 'The fixture unexpectedly changed topology'
print('REPLAY_TOPOLOGY_UNCHANGED', flush=True)
assert all(masks_after['replay_valid']), 'Changing only extrusion Offset invalidated the downstream selection'
assert masks_after['replay_points'] == masks_before['replay_points'], 'Captured membership changed'
for index, (a, b) in enumerate(zip(after[0], before[0])):
    expected = b + delta if index in selected else b
    assert (a - expected).length < 2e-5, ('Scale pivot failed to follow extrusion', index, a, expected)

# A subsequent move/rotation must also retain its own selection and moving pivot.
group.nodes.active = viewer
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.node.activate_viewer()
    bpy.context.view_layer.update()
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.mesh.select_mode(type='FACE')
bm = bmesh.from_edit_mesh(bpy.context.object.data)
bm.faces.ensure_lookup_table()
face_index = next(i for i, vertices in enumerate(stage_after[2]) if set(vertices) == selected)
face = bm.faces[face_index]
face.select_set(True)
bm.faces.active = face
bm.select_history.add(face)
bm.select_flush_mode()
bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
bm = face = None
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=(0.1, 0, 0), use_proportional_edit=False,
                                        mirror=False) == {'FINISHED'}
refresh()
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.rotate(value=0.3, orient_type='GLOBAL', orient_axis='Z',
                                    constraint_axis=(False, False, True),
                                    use_proportional_edit=False, mirror=False) == {'FINISHED'}
refresh()
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
rotation = next(l.from_node for l in group.links if l.to_node == viewer)
rotation_output = next(s for s in rotation.outputs if s.type == 'GEOMETRY')
mixed_before, _ = evaluate(rotation_output)
offset.vector = Vector(offset.vector) + delta
mixed_after, _ = evaluate(rotation_output)
assert mixed_before[1:] == mixed_after[1:]
for i, (a, b) in enumerate(zip(mixed_after[0], mixed_before[0])):
    assert (a - b - (delta if i in selected else Vector())).length < 3e-5, (
        'Mixed E/S/G/R replay', i, a, b)
print('GEOMETRY_VIEWER_SELECTION_REPLAY_SMOKE_OK', flush=True)
