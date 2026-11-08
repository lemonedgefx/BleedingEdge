# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Viewer Delete/Dissolve: eight recipes, guards, continuation, undo and persistence.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
Reference results use ordinary nodes with an independently specified index selection.
"""

import os
import tempfile

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
SOURCE = TREE = 'Viewer Delete Test'
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


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer()
                and l.to_socket.type == 'GEOMETRY')


def node_call(action='VALIDATE', **kwargs):
    with bpy.context.temp_override(**override(NODE_AREA)):
        return bpy.ops.node.geometry_viewer_edit_transaction(action=action, **kwargs)


def delete(action):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        result = bpy.ops.node.geometry_viewer_edit_delete(type=action)
    if result == {'FINISHED'} and bpy.app.background:
        # Script execution suppresses automatic operator undo in background mode. Real-input
        # single-step undo is verified independently by the GUI popup test.
        with bpy.context.temp_override(**override(NODE_AREA)):
            bpy.ops.ed.undo_push(message='Scripted Viewer Delete / Dissolve')
    return result


def mesh_snapshot():
    bpy.context.view_layer.update()
    evaluated = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh(preserve_all_data_layers=True,
                             depsgraph=bpy.context.evaluated_depsgraph_get())
    result = dict(points=[tuple(round(c, 5) for c in v.co) for v in mesh.vertices],
                  edges=[tuple(e.vertices) for e in mesh.edges],
                  faces=[tuple(f.vertices) for f in mesh.polygons],
                  material=[f.material_index for f in mesh.polygons])
    for name in ('source_point', 'source_edge', 'source_face'):
        if name in mesh.attributes:
            result[name] = [round(v.value, 5) for v in mesh.attributes[name].data]
    if mesh.uv_layers:
        result['uv'] = [tuple(round(c, 5) for c in v.uv) for v in mesh.uv_layers[0].data]
    evaluated.to_mesh_clear()
    return result


def evaluate_branch():
    group = tree()
    input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    operation = result_node()
    group.links.new(next(s for s in operation.outputs if s.type == 'GEOMETRY'), output.inputs['Geometry'])
    try:
        return mesh_snapshot()
    finally:
        group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        bpy.context.view_layer.update()


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def end():
    if bpy.context.mode == 'EDIT_MESH':
        with bpy.context.temp_override(**override(NODE_AREA)):
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
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_retired(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count, len(tree().nodes)
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def setup(grid=False):
    end()
    old = bpy.data.objects.get(SOURCE)
    if old:
        old.modifiers.clear()
        bpy.data.objects.remove(old, do_unlink=True)
        bpy.data.node_groups.remove(tree())
    mesh = bpy.data.meshes.new(SOURCE)
    if grid:
        mesh.from_pydata([(x, y, 0) for y in range(3) for x in range(3)], [],
                         [(x + y * 3, x + y * 3 + 1, x + y * 3 + 4, x + y * 3 + 3)
                          for y in range(2) for x in range(2)])
    else:
        mesh.from_pydata([(x, y, 0) for y in range(2) for x in range(4)], [],
                         [(x, x + 1, x + 5, x + 4) for x in range(3)])
    for domain, name in (('POINT', 'source_point'), ('EDGE', 'source_edge'), ('FACE', 'source_face')):
        attribute = mesh.attributes.new(name, 'FLOAT', domain)
        for i, value in enumerate(attribute.data):
            value.value = i * 1.25 + 0.125
    uv = mesh.uv_layers.new(name='UVMap')
    for i, value in enumerate(uv.data):
        value.uv = (i * 0.07, i * 0.11 + 0.25)
    for _ in range(2):
        mesh.materials.append(bpy.data.materials.new('Viewer Delete Material'))
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
    input_node = group.nodes.new('NodeGroupInput')
    output = group.nodes.new('NodeGroupOutput')
    node = group.nodes.new('GeometryNodeViewer')
    node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
    group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
    obj.modifiers.new(TREE, 'NODES').node_group = group
    # Background runs have no editor refresh event. Initialize the editor after its context exists.
    NODE_AREA.type = 'DOPESHEET_EDITOR'
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Viewer Delete Baseline')


def reference(domain, indices, dissolving=False, mode='ALL'):
    group = tree()
    input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    operation = group.nodes.new('GeometryNodeDissolveElements' if dissolving else 'GeometryNodeDeleteGeometry')
    operation.domain = domain
    if not dissolving:
        operation.mode = mode
    selection = group.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = domain
    selection.inputs['Indices'].default_value = ', '.join(map(str, indices))
    group.links.new(input_node.outputs['Geometry'], next(s for s in operation.inputs if s.type == 'GEOMETRY'))
    group.links.new(selection.outputs['Selection'], operation.inputs['Selection'])
    group.links.new(next(s for s in operation.outputs if s.type == 'GEOMETRY'), output.inputs['Geometry'])
    try:
        return mesh_snapshot()
    finally:
        group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        group.nodes.remove(operation)
        group.nodes.remove(selection)
        bpy.context.view_layer.update()


CASES = (
    ('VERT', 'POINT', [1], False, 'ALL'),
    ('EDGE', 'EDGE', None, False, 'ALL'),
    ('FACE', 'FACE', [0], False, 'ALL'),
    ('EDGE_FACE', 'EDGE', None, False, 'EDGE_FACE'),
    ('ONLY_FACE', 'FACE', [0], False, 'ONLY_FACE'),
    ('DISSOLVE_VERT', 'POINT', [4], True, 'ALL'),
    ('DISSOLVE_EDGE', 'EDGE', None, True, 'ALL'),
    ('DISSOLVE_FACE', 'FACE', [0, 1], True, 'ALL'),
)
for action, domain, indices, dissolving, mode in CASES:
    setup(grid=action == 'DISSOLVE_VERT')
    if indices is None:
        indices = [next(e.index for e in source().data.edges if set(e.vertices) == {1, 5})]
    original = mesh_snapshot()
    expected = reference(domain, indices, dissolving, mode)
    assert expected != original, action
    begin()
    select(domain, indices)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_delete.poll()
        assert not bpy.ops.mesh.delete.poll() and not bpy.ops.mesh.dissolve_mode.poll()
    assert delete(action) == {'FINISHED'}, action
    assert_retired(5)
    operation = result_node()
    assert operation.bl_idname == ('GeometryNodeDissolveElements' if dissolving else 'GeometryNodeDeleteGeometry')
    assert operation.domain == domain
    if not dissolving:
        assert operation.mode == mode
    capture = next(l.from_node for l in tree().links if l.to_node == operation and l.to_socket.name == 'Selection')
    assert capture.domain == domain and capture.selected_count == len(indices)
    operation = capture = None  # Undo replaces datablocks.
    assert evaluate_branch() == expected, action
    assert mesh_snapshot() == original, 'The Group Output/source branch must remain unchanged'
    assert node_call() == {'FINISHED'}
    assert bpy.context.mode == 'EDIT_MESH'
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    assert tuple(map(len, (bm.verts, bm.edges, bm.faces))) == tuple(len(expected[k]) for k in ('points', 'edges', 'faces'))
    assert not any(e.select for elements in (bm.verts, bm.edges, bm.faces) for e in elements)
    assert not bm.select_history
    bm = None
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.ed.undo() == {'FINISHED'}
    assert_retired(3)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.ed.redo() == {'FINISHED'}
    assert_retired(5)
    assert evaluate_branch() == expected
    checks += 1

setup()
begin()
select('POINT', [])
for action, *_ in CASES:
    assert delete(action) == {'CANCELLED'}
    assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
    checks += 1
boundary = next(e.index for e in source().data.edges if set(e.vertices) == {0, 4})
select('EDGE', [boundary])
assert delete('DISSOLVE_EDGE') == {'CANCELLED'}  # A boundary edge has no adjacent face pair to merge.
assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
checks += 1
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.select_mode(type='VERT', use_extend=True)
assert delete('EDGE') == {'CANCELLED'}  # Mixed picking domains are not implicitly guessed.
assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
checks += 1
select('FACE', [0])
assert delete('DISSOLVE_FACE') == {'CANCELLED'}  # One face cannot form a merged region.
assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
assert len([f for f in bmesh.from_edit_mesh(bpy.context.object.data).faces if f.select]) == 1
checks += 1
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.hide(unselected=False)
assert delete('FACE') == {'CANCELLED'}
assert len(tree().nodes) == 3
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.reveal(select=False)
select('FACE', [0])
transaction = bpy.ops.node.geometry_viewer_edit_transaction.get_rna_type()
assert 'offset' not in transaction.properties  # The numeric-preview feature is removed.
assert {item.identifier for item in transaction.properties['action'].enum_items} == {
    'VALIDATE', 'ABORT_EXTRUSION', 'ABORT_LOOP_CUT', 'ABORT_DUPLICATION'}
checks += 2

# An explicit menu domain can differ from picking mode: selected faces flush to their vertices.
select('FACE', [0])
assert delete('VERT') == {'FINISHED'}
assert_retired(5)
assert result_node().domain == 'POINT'
assert node_call() == {'FINISHED'}
assert bpy.context.tool_settings.mesh_select_mode[:] == (False, False, True)
checks += 1

# A cut -> dissolve -> delete chain captures a fresh input stage for each action.
setup()
begin()
seed = next(e.index for e in source().data.edges if set(e.vertices) == {0, 4})
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=seed, cuts=1) == {'FINISHED'}
assert node_call() == {'FINISHED'}
assert delete('DISSOLVE_EDGE') == {'FINISHED'}
assert_retired(7)
assert node_call() == {'FINISHED'}
select('FACE', [0])
assert delete('ONLY_FACE') == {'FINISHED'}
assert_retired(9)
expected = evaluate_branch()
checks += 1
with tempfile.TemporaryDirectory(prefix='viewer-delete-') as directory:
    path = os.path.join(directory, 'delete.blend')
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    assert len(tree().nodes) == 9
    assert evaluate_branch() == expected
checks += 1

# Deleting all faces with All mode commits an empty graph and exits editing safely.
NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'NODE_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
setup()
begin()
select('FACE', [0, 1, 2])
assert delete('FACE') == {'FINISHED'}
assert_retired(5)
assert not evaluate_branch()['points']
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
assert_retired(3)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.redo() == {'FINISHED'}
assert_retired(5)
checks += 1
assert not bpy.app.build_options.bex
print('GEOMETRY_VIEWER_DELETE_SMOKE_OK', checks)
