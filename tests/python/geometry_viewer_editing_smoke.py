# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unified selection, native editing, identity guards, copying and persistence.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
Numeric Preview Offset was removed; native modal coverage lives in the UI suites.
"""

import os
import tempfile

import bmesh
import bpy

NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = 'Viewer Edit Test Source'
TREE = 'Viewer Edit Test Tree'


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def source():
    return bpy.data.objects[SOURCE]


def tree():
    return bpy.data.node_groups[TREE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def capture():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex' and
                n.mode == 'CAPTURED')


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    assert bpy.context.mode == 'EDIT_MESH'


def select(domain, index=0):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    elements = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    if index is None:
        for element in elements:
            element.select_set(element.index % 2 == 0)
    else:
        elements[index].select_set(True)
        bm.select_history.add(elements[index])
    bm.select_flush_mode()
    points = {v.index for v in bm.verts if v.select}
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
    return points


def import_selection():
    with bpy.context.temp_override(**override(NODE_AREA)):
        return bpy.ops.node.geometry_viewer_import_selection()


def clean():
    assert bpy.context.mode == 'OBJECT'
    assert bpy.context.object == source()
    assert not any(o.name.startswith(('Viewer Editing', 'Viewer Selection')) for o in bpy.data.objects)
    assert not any(m.name.startswith(('Viewer Editing', 'Viewer Selection')) for m in bpy.data.meshes)


def masks(node):
    group = node.id_data
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
    old = next(l.from_socket for l in group.links if l.to_node == output)
    geometry = input_node.outputs['Geometry']
    stores = []
    for name, domain in (('Selection', node.domain), ('Points', 'POINT'), ('Valid', 'POINT')):
        store = group.nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type = 'BOOLEAN'
        store.domain = domain
        store.inputs['Name'].default_value = name
        group.links.new(geometry, store.inputs['Geometry'])
        group.links.new(node.outputs[name], store.inputs['Value'])
        geometry = store.outputs['Geometry']
        stores.append(store)
    group.links.new(geometry, output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = source().evaluated_get(depsgraph)
        mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
        result = {name: [v.value for v in mesh.attributes[name].data]
                  for name in ('Selection', 'Points', 'Valid')}
        evaluated.to_mesh_clear()
        return result
    finally:
        group.links.new(old, output.inputs['Geometry'])
        for store in stores:
            group.nodes.remove(store)


bpy.ops.mesh.primitive_cube_add()
source_object = bpy.context.object
source_object.name = SOURCE
node_group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
node_group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
node_group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
group_input = node_group.nodes.new('NodeGroupInput')
group_output = node_group.nodes.new('NodeGroupOutput')
viewer_node = node_group.nodes.new('GeometryNodeViewer')
viewer_node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
node_group.links.new(group_input.outputs['Geometry'], group_output.inputs['Geometry'])
node_group.links.new(group_input.outputs['Geometry'], viewer_node.inputs['Geometry'])
source_object.modifiers.new('Viewer Edit Test', 'NODES').node_group = node_group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
source_object = node_group = group_input = group_output = viewer_node = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Edit Test Baseline')

# Empty import is guarded in execution, not just by a grey button.
begin()
assert import_selection() == {'CANCELLED'} and len(tree().nodes) == 3
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert not bpy.ops.mesh.duplicate.poll()
    assert not bpy.ops.mesh.extrude_region_move.poll()
    assert bpy.ops.transform.translate.poll()
    assert bpy.ops.node.geometry_viewer_edit_extrude.poll()
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
clean()

for domain in ('VERT', 'EDGE', 'FACE'):
    begin()
    points = select(domain)
    assert import_selection() == {'FINISHED'}
    clean()
    node = capture()
    assert node.selected_count == 1
    assert node.inputs[0].identifier == 'Indices' and node.inputs[0].default_value == '0'
    result = masks(node)
    assert sum(result['Selection']) == 1 and all(result['Valid'])
    assert {i for i, value in enumerate(result['Points']) if value} == points
    original_domain = node.domain
    node.domain = 'POINT'
    assert node.domain == original_domain
    node.mode = 'MANUAL'
    node.domain = 'POINT'
    node.mode = 'CAPTURED'
    assert node.domain == original_domain and masks(node) == result
    tree().nodes.remove(node)

# Manual nodes remain geometry-independent and retain the original parser.
node = tree().nodes.new('GeometryNodeSelectByIndex')
assert node.mode == 'MANUAL' and node.inputs[1].identifier == 'Geometry' and node.inputs[1].is_unavailable
for domain in ('POINT', 'EDGE', 'FACE'):
    node.domain = domain
    node.inputs['Indices'].default_value = '0, 2-3, 999'
    result = masks(node)
    assert sum(result['Selection']) == 3 and all(result['Valid'])
    if domain == 'POINT':
        expected = {0, 2, 3}
    else:
        elements = source().data.edges if domain == 'EDGE' else source().data.polygons
        expected = {v for i in (0, 2, 3) for v in elements[i].vertices}
    assert {i for i, value in enumerate(result['Points']) if value} == expected
node.inputs['Indices'].default_value = '0, bad, 2-3'
assert sum(masks(node)['Selection']) == 3 and not any(masks(node)['Valid'])
node.mode = 'CAPTURED'
assert not any(masks(node)['Valid'])
tree().nodes.remove(node)
print('VIEWER_UNIFIED_MANUAL_CAPTURE_OK', flush=True)

# Native movement owns one graph transaction and preserves the source/output.
begin()
select('FACE')
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=(0.25, -0.3, 0.5), use_proportional_edit=False,
                                        mirror=False) == {'FINISHED'}
clean()
assert len(tree().nodes) == 5 and capture().selected_count == 1
assert capture().inputs[0].default_value == '0'
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Confirmed Viewer move')
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
clean()
assert len(tree().nodes) == 3
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.ed.redo() == {'FINISHED'}
clean()
assert len(tree().nodes) == 5 and capture().selected_count == 1
print('VIEWER_EDIT_TRANSACTION_UNDO_OK', flush=True)

# Active sessions still use exact snapshots, not the relaxed replay contract.
begin()
select('EDGE')
original = source().data.vertices[0].co.copy()
source().data.vertices[0].co.x += 0.123
source().data.update()
with bpy.context.temp_override(**override(NODE_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}
clean()
source().data.vertices[0].co = original
source().data.update()
for node in list(tree().nodes):
    if node.bl_idname in {'GeometryNodeSelectByIndex', 'GeometryNodeSetPosition'}:
        tree().nodes.remove(node)
input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
tree().links.new(input_node.outputs['Geometry'], viewer().inputs['Geometry'])
print('VIEWER_EDIT_INVALIDATION_OK', flush=True)

# Connected geometry accepts deformation, but not symmetric coordinate swaps or
# same-count connectivity changes. Counts alone are not a capture identity.
begin()
select('FACE')
assert import_selection() == {'FINISHED'}
clean()
connected_mask = masks(capture())
positions = [v.co.copy() for v in source().data.vertices]
edges = [tuple(e.vertices) for e in source().data.edges]
faces = [tuple(f.vertices) for f in source().data.polygons]
source().data.vertices[0].co.x += 0.123
source().data.update()
assert masks(capture()) == connected_mask
source().data.vertices[0].co = positions[1]
source().data.vertices[1].co = positions[0]
source().data.update()
assert not any(masks(capture())['Valid'])
source().data.vertices[0].co = positions[0]
source().data.vertices[1].co = positions[1]
source().data.update()
assert masks(capture()) == connected_mask
source().data.clear_geometry()
source().data.from_pydata(positions, edges, list(reversed(faces)))
assert len(source().data.vertices) == len(positions) and len(source().data.polygons) == len(faces)
assert not any(masks(capture())['Valid'])
source().data.clear_geometry()
source().data.from_pydata(positions, edges, faces)
assert masks(capture()) == connected_mask
tree().nodes.remove(capture())
print('VIEWER_CAPTURE_DEFORMATION_REORDER_GUARDS_OK', flush=True)

# Isolated points stay strict without IDs, even with unchanged topology.
source().data.clear_geometry()
source().data.from_pydata([(i * 0.25, 0, 0) for i in range(4096)], [], [])
begin()
select('VERT', None)
assert import_selection() == {'FINISHED'}
assert capture().selected_count == 2048 and all(masks(capture())['Valid'])
assert capture().inputs[0].default_value == ''
original = source().data.vertices[0].co.copy()
source().data.vertices[0].co.z += 1
source().data.update()
assert not any(masks(capture())['Valid'])
source().data.vertices[0].co = original
source().data.update()
tree().nodes.remove(capture())

# Unique IDs permit deformation and reject reordered/duplicate IDs.
ids = source().data.attributes.new('id', 'INT', 'POINT')
for i, value in enumerate(ids.data):
    value.value = i
source().data.update()
begin()
select('VERT', None)
assert import_selection() == {'FINISHED'}
clean()
for vertex in source().data.vertices:
    vertex.co.z += 1
source().data.update()
expected = masks(capture())
assert all(expected['Valid']) and sum(expected['Points']) == 2048
ids = source().data.attributes['id']
ids.data[0].value, ids.data[1].value = 1, 0
source().data.update()
assert not any(masks(capture())['Valid'])
ids.data[0].value, ids.data[1].value = 0, 0
source().data.update()
assert not any(masks(capture())['Valid'])
ids.data[1].value = 1
source().data.update()
copy = tree().copy()
modifier = source().modifiers[0]
modifier.node_group = copy
copy_node = next(n for n in copy.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
assert copy_node.selected_count == 2048 and masks(copy_node) == expected
modifier.node_group = tree()
bpy.data.node_groups.remove(copy)
copy = copy_node = modifier = ids = None
with tempfile.TemporaryDirectory(prefix='viewer-edit-test-') as directory:
    path = os.path.join(directory, 'capture.blend')
    begin()
    bpy.ops.wm.save_as_mainfile(filepath=path)
    clean()
    bpy.ops.wm.open_mainfile(filepath=path)
    clean()
    assert capture().selected_count == 2048 and masks(capture()) == expected
print('VIEWER_EDIT_LARGE_CAPTURE_SAVE_RELOAD_OK', flush=True)
print('GEOMETRY_VIEWER_EDITING_SMOKE_OK', flush=True)

