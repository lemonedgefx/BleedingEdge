# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Viewer Merge/Duplicate adapters: replay, domain conversion, continuation and persistence.

Requires a build containing the new adapters; do not run this on the previous executable.
Run --background --factory-startup --python-exit-code 1 --python this_file.py.
"""

import os
import tempfile

import bmesh
import bpy
from mathutils import Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Merge Duplicate Test'
checks = 0


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def source():
    return bpy.data.objects[SOURCE]


def tree():
    return bpy.data.node_groups[TREE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def node_call(action='VALIDATE', **kwargs):
    with bpy.context.temp_override(**override(NODE_AREA)):
        return bpy.ops.node.geometry_viewer_edit_transaction(action=action, **kwargs)


def view_call(name, **kwargs):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        return getattr(bpy.ops.node, name)(**kwargs)


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer() and l.to_socket.type == 'GEOMETRY')


def snapshot():
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = source().evaluated_get(depsgraph)
    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    result = dict(points=[tuple(round(c, 5) for c in v.co) for v in mesh.vertices],
                  edges=[tuple(e.vertices) for e in mesh.edges],
                  faces=[tuple(f.vertices) for f in mesh.polygons],
                  materials=[f.material_index for f in mesh.polygons],
                  uv=[tuple(round(c, 5) for c in v.uv) for v in mesh.uv_layers['UVMap'].data])
    for name in ('source_point', 'source_edge', 'source_face'):
        result[name] = [v.value for v in mesh.attributes[name].data]
    evaluated.to_mesh_clear()
    return result


def branch_snapshot():
    group = tree()
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    stage = result_node()
    group.links.new(next(s for s in stage.outputs if s.type == 'GEOMETRY'), output.inputs['Geometry'])
    try:
        return snapshot()
    finally:
        input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
        group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        bpy.context.view_layer.update()


def end():
    if bpy.context.mode == 'EDIT_MESH':
        tree().nodes.active = viewer()
        with bpy.context.temp_override(**override(NODE_AREA)):
            assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def select(domain, indices):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type={'POINT': 'VERT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain])
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    elements = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    for index in indices:
        elements[index].select_set(True)
        bm.select_history.add(elements[index])
    if domain == 'FACE' and indices:
        bm.faces.active = bm.faces[indices[-1]]
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def affected_points(domain, indices):
    mesh = source().data
    if domain == 'POINT':
        return indices
    elements = mesh.edges if domain == 'EDGE' else mesh.polygons
    return sorted({v for i in indices for v in elements[i].vertices})


def push(message):
    # Python background EXEC calls suppress automatic operator undo. Real events are tested
    # separately; this snapshot must be taken only after the adapter has retired its proxy.
    if bpy.app.background:
        with bpy.context.temp_override(**override(NODE_AREA)):
            bpy.ops.ed.undo_push(message=message)


def assert_retired(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count, len(tree().nodes)
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)


def setup(near=False):
    end()
    if bpy.data.objects.get(SOURCE):
        source().modifiers.clear()
        bpy.data.objects.remove(source(), do_unlink=True)
        bpy.data.node_groups.remove(tree())
    mesh = bpy.data.meshes.new(SOURCE)
    points = [(x, y, 0) for y in range(2) for x in range(4)] + [(5, 0, 0), (6, 0, 0), (8, 0, 0)]
    if near:
        points[1], points[8] = (0.0004, 0, 0), (0.0006, 0, 0)
    mesh.from_pydata(points, [(8, 9)], [(x, x + 1, x + 5, x + 4) for x in range(3)])
    for domain, name in (('POINT', 'source_point'), ('EDGE', 'source_edge'), ('FACE', 'source_face')):
        attr = mesh.attributes.new(name, 'INT', domain)
        for i, value in enumerate(attr.data):
            value.value = i + 100
    uv = mesh.uv_layers.new(name='UVMap')
    for i, value in enumerate(uv.data):
        value.uv = (i * 0.0625, i * 0.125 + 0.25)
    for _ in range(2):
        mesh.materials.append(bpy.data.materials.new('Merge Duplicate Material'))
    for face in mesh.polygons:
        face.material_index = face.index % 2
    obj = bpy.data.objects.new(SOURCE, mesh)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    obj.rotation_euler, obj.scale = (0.2, -0.4, 0.3), (1.5, 0.7, 2)
    group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    input_node, output = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
    node = group.nodes.new('GeometryNodeViewer')
    node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
    input_node.location, node.location = (0, 0), (500, 0)
    group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
    obj.modifiers.new(TREE, 'NODES').node_group = group
    NODE_AREA.type = 'DOPESHEET_EDITOR'
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Viewer Merge Duplicate Baseline')


def reference(domain, indices, mode=None, offset=(0, 0, 0)):
    group = tree()
    input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    before = set(group.nodes)
    selection = group.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = 'POINT' if mode else domain
    selection.inputs['Indices'].default_value = ', '.join(
        map(str, affected_points(domain, indices) if mode else indices))
    if mode:
        stage = group.nodes.new('GeometryNodeMergeByDistance')
        stage.inputs['Mode'].default_value = mode
        stage.inputs['Distance'].default_value = 0.001
        group.links.new(input_node.outputs['Geometry'], stage.inputs['Geometry'])
        group.links.new(selection.outputs['Selection'], stage.inputs['Selection'])
    else:
        separate = group.nodes.new('GeometryNodeSeparateGeometry')
        separate.domain = domain
        move = group.nodes.new('GeometryNodeSetPosition')
        move.inputs['Offset'].default_value = source().matrix_world.inverted().to_3x3() @ Vector(offset)
        stage = group.nodes.new('GeometryNodeJoinGeometry')
        group.links.new(input_node.outputs['Geometry'], separate.inputs['Geometry'])
        group.links.new(selection.outputs['Selection'], separate.inputs['Selection'])
        group.links.new(separate.outputs['Selection'], move.inputs['Geometry'])
        group.links.new(move.outputs['Geometry'], stage.inputs['Geometry'])
        group.links.new(input_node.outputs['Geometry'], stage.inputs['Geometry'])
    group.links.new(stage.outputs['Geometry'], output.inputs['Geometry'])
    try:
        return snapshot()
    finally:
        group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        for node in list(group.nodes):
            if node not in before:
                group.nodes.remove(node)
        bpy.context.view_layer.update()


for mode, menu in (('ALL', 'All'), ('CONNECTED', 'Connected'), ('NOT_CONNECTED', 'Not Connected')):
    for domain, indices in (('POINT', [0, 1, 8]), ('EDGE', None), ('FACE', [0])):
        setup(near=True)
        if indices is None:
            indices = [e.index for e in source().data.edges if set(e.vertices) in ({0, 1}, {8, 9})]
        original = snapshot()
        expected = reference(domain, indices, mode=menu)
        begin()
        select(domain, indices)
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.node.geometry_viewer_edit_merge.poll()
            assert not bpy.ops.mesh.remove_doubles.poll() and not bpy.ops.mesh.merge.poll()
        changed = len(expected['points']) != len(original['points'])
        assert view_call('geometry_viewer_edit_merge', mode=mode, distance=0.001) == (
            {'FINISHED'} if changed else {'CANCELLED'})
        if not changed:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            end()
            checks += 1
            continue
        push('Scripted Viewer Merge')
        assert_retired(5)
        operation = result_node()
        assert operation.bl_idname == 'GeometryNodeMergeByDistance'
        assert operation.inputs['Mode'].default_value == menu
        capture = next(l.from_node for l in tree().links if l.to_node == operation and l.to_socket.name == 'Selection')
        assert capture.domain == domain and capture.selected_count == len(indices)
        assert any(l.from_node == capture and l.from_socket.name == 'Points' and l.to_node == operation
                   for l in tree().links)
        assert branch_snapshot() == expected and snapshot() == original
        operation = capture = None
        assert node_call() == {'FINISHED'} and bpy.context.mode == 'EDIT_MESH'
        bm = bmesh.from_edit_mesh(bpy.context.object.data)
        assert len(bm.verts) == len(expected['points'])
        assert not any(e.select for elements in (bm.verts, bm.edges, bm.faces) for e in elements)
        assert not bm.select_history
        bm = None
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.ed.undo() == {'FINISHED'}
        assert_retired(3)
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.ed.redo() == {'FINISHED'}
        assert_retired(5)
        assert branch_snapshot() == expected
        checks += 1


for domain, indices in (('POINT', [8, 9, 10]), ('EDGE', None), ('FACE', [0, 1])):
    for offset in ((0, 0, 0), (0.25, -0.5, 1.25)):
        setup()
        if indices is None:
            indices = [e.index for e in source().data.edges if set(e.vertices) in ({0, 1}, {1, 5})]
        original = snapshot()
        expected = reference(domain, indices, offset=offset)
        begin()
        select(domain, indices)
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.node.geometry_viewer_edit_duplicate.poll()
            assert not bpy.ops.mesh.duplicate.poll()
        assert view_call('geometry_viewer_edit_duplicate', offset=offset) == {'FINISHED'}
        push('Scripted Viewer Duplicate')
        assert_retired(7)
        assert result_node().bl_idname == 'GeometryNodeJoinGeometry'
        assert branch_snapshot() == expected and snapshot() == original
        capture = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
        assert capture.domain == domain and capture.selected_count == len(indices)
        assert not any(n.bl_idname == 'GeometryNodeDuplicateElements' for n in tree().nodes)
        ordered = [capture, next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSeparateGeometry'),
                   next(n for n in tree().nodes if n.label == 'Duplicate Offset'), result_node(), viewer()]
        assert all(a.location_absolute.x + a.width < b.location_absolute.x for a, b in zip(ordered, ordered[1:]))
        ordered = capture = None
        assert node_call() == {'FINISHED'} and bpy.context.mode == 'EDIT_MESH'
        bm = bmesh.from_edit_mesh(bpy.context.object.data)
        elements = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
        assert len([e for e in elements if e.select]) == len(indices)
        assert bm.select_history.active is not None
        assert len(bm.verts) == len(expected['points'])
        bm = None
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.ed.undo() == {'FINISHED'}
        assert_retired(3)
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.ed.redo() == {'FINISHED'}
        assert_retired(7)
        assert branch_snapshot() == expected
        checks += 1


# Empty and locally hidden selections produce no transaction and keep the picking session.
for operation in ('geometry_viewer_edit_merge', 'geometry_viewer_edit_duplicate'):
    setup(near=True)
    begin()
    select('POINT', [])
    assert view_call(operation) == {'CANCELLED'}
    assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
    select('POINT', [0, 1])
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.verts.ensure_lookup_table()
    bm.verts[10].hide_set(True)
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
    bm = None
    assert view_call(operation) == {'CANCELLED'}
    assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
    end()
    checks += 2


# A chained copy can be transformed or deleted, and normal graph replay requires no proxy.
setup()
original = snapshot()
begin()
select('FACE', [0, 1])
assert view_call('geometry_viewer_edit_duplicate', offset=(0, 0, 1)) == {'FINISHED'}
push('Scripted Viewer Duplicate Chain')
assert node_call() == {'FINISHED'}
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.1, 1.1, 1.1)) == {'FINISHED'}
push('Scripted Viewer Duplicate Scale')
assert node_call() == {'FINISHED'}
assert view_call('geometry_viewer_edit_duplicate', offset=(1, 0, 0)) == {'FINISHED'}
push('Scripted Viewer Duplicate Again')
assert node_call() == {'FINISHED'}
assert view_call('geometry_viewer_edit_delete', type='FACE') == {'FINISHED'}
push('Scripted Viewer Delete Copies')
expected = branch_snapshot()
assert snapshot() == original
with tempfile.TemporaryDirectory(prefix='viewer-merge-duplicate-') as directory:
    path = os.path.join(directory, 'merge-duplicate.blend')
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    assert bpy.context.mode == 'OBJECT'
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)
    assert branch_snapshot() == expected and snapshot() == original
checks += 2
print('GEOMETRY_VIEWER_MERGE_DUPLICATE_SMOKE_OK', checks)
