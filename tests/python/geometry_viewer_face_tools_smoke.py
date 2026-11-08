# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Viewer face tools: recipes, native parity, selection, guards, undo and persistence.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
The reference graph uses manual indices, independently of the captured adapter.
"""

import os
import tempfile

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Face Tools Test'
checks = 0
OP_TYPES = dict(TRIANGULATE='GeometryNodeTriangulate', FLIP='GeometryNodeFlipFaces',
                SMOOTH='GeometryNodeSetShadeSmooth', FLAT='GeometryNodeSetShadeSmooth')
QUAD_NAMES = dict(BEAUTY='Beauty', FIXED='Fixed', FIXED_ALTERNATE='Fixed Alternate',
                  SHORTEST_DIAGONAL='Shortest Diagonal', LONGEST_DIAGONAL='Longest Diagonal')
NGON_NAMES = dict(BEAUTY='Beauty', CLIP='Clip')


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


def snapshot(mesh):
    return dict(points=[tuple(round(c, 5) for c in v.co) for v in mesh.vertices],
                edges=[tuple(e.vertices) for e in mesh.edges],
                faces=[tuple(f.vertices) for f in mesh.polygons],
                material=[f.material_index for f in mesh.polygons],
                smooth=[f.use_smooth for f in mesh.polygons],
                point=[round(d.value, 5) for d in mesh.attributes['source_point'].data],
                edge=[round(d.value, 5) for d in mesh.attributes['source_edge'].data],
                face=[round(d.value, 5) for d in mesh.attributes['source_face'].data],
                uv=[tuple(round(c, 5) for c in d.uv) for d in mesh.uv_layers[0].data],
                protected=[d.value for d in mesh.attributes['.viewer_face_source_existing'].data])


def canonical(data):
    """Ignore domain ordering, but retain winding, UV correspondence and all attributes."""
    faces, corner = [], 0
    for i, verts in enumerate(data['faces']):
        pairs = tuple(zip(verts, data['uv'][corner:corner + len(verts)]))
        cycle = min(pairs[j:] + pairs[:j] for j in range(len(pairs)))
        faces.append((cycle, data['material'][i], data['smooth'][i], data['face'][i], data['protected'][i]))
        corner += len(verts)
    edges = sorted((tuple(sorted(e)), value) for e, value in zip(data['edges'], data['edge']))
    return data['points'], data['point'], edges, sorted(faces)


def mesh_snapshot():
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = source().evaluated_get(depsgraph)
    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    try:
        assert not any(a.name.startswith('.viewer_face_source_') and
                       a.name != '.viewer_face_source_existing' for a in mesh.attributes)
        return snapshot(mesh)
    finally:
        evaluated.to_mesh_clear()


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
        tree().nodes.active = viewer()
        with bpy.context.temp_override(**override(NODE_AREA)):
            assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def select(indices, domain='FACE'):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=dict(FACE='FACE', EDGE='EDGE', POINT='VERT')[domain])
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    elements = dict(FACE=bm.faces, EDGE=bm.edges, POINT=bm.verts)[domain]
    for index in indices:
        elements[index].select_set(True)
        bm.select_history.add(elements[index])
    if indices and domain == 'FACE':
        bm.faces.active = elements[indices[-1]]
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def face_tool(action, **kwargs):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        result = bpy.ops.node.geometry_viewer_edit_face(type=action, **kwargs)
    if result == {'FINISHED'} and bpy.app.background:
        # Native input owns automatic undo; scripted background execution does not.
        with bpy.context.temp_override(**override(NODE_AREA)):
            bpy.ops.ed.undo_push(message='Scripted Viewer Face Tool')
    return result


def assert_retired(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count, len(tree().nodes)
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname in ('GeometryNodeStoreNamedAttribute', 'GeometryNodeInputIndex')
                   for n in tree().nodes)


def setup():
    end()
    old = bpy.data.objects.get(SOURCE)
    if old:
        old.modifiers.clear()
        bpy.data.objects.remove(old, do_unlink=True)
        bpy.data.node_groups.remove(tree())
    mesh = bpy.data.meshes.new(SOURCE)
    # Two adjacent nonplanar quads, a concave n-gon, a triangle and a loose edge/point.
    mesh.from_pydata([(0, 0, 0), (1.2, 0, 0), (2, 0, 0.1), (0, 1, 0.1),
                      (1, 1.3, 0), (2.2, 1, 0), (3, 0, 0), (5, 0, 0),
                      (5, 2, 0), (4, 0.6, 0), (3, 2, 0), (6, 0, 0),
                      (7, 0, 0), (6.3, 1, 0), (8, 0, 0), (9, 0, 0), (10, 0, 0)],
                     [(14, 15)], [(0, 1, 4, 3), (1, 2, 5, 4), (6, 7, 8, 9, 10), (11, 12, 13)])
    for domain, name in (('POINT', 'source_point'), ('EDGE', 'source_edge'), ('FACE', 'source_face')):
        attribute = mesh.attributes.new(name, 'FLOAT', domain)
        for i, value in enumerate(attribute.data):
            value.value = i * 1.25 + 0.125
    protected = mesh.attributes.new('.viewer_face_source_existing', 'INT', 'FACE')
    for i, value in enumerate(protected.data):
        value.value = 40 + i
    uv = mesh.uv_layers.new(name='UVMap')
    for i, value in enumerate(uv.data):
        value.uv = (i * 0.07, i * 0.11 + 0.25)
    for _ in range(2):
        mesh.materials.append(bpy.data.materials.new('Viewer Face Material'))
    for face in mesh.polygons:
        face.material_index, face.use_smooth = face.index % 2, bool(face.index % 2)
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
    group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
    obj.modifiers.new(TREE, 'NODES').node_group = group
    NODE_AREA.type = 'DOPESHEET_EDITOR'
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Viewer Face Tools Baseline')


def reference(action, indices, quad='BEAUTY', ngon='BEAUTY'):
    group = tree()
    input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
    output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
    operation = group.nodes.new(OP_TYPES[action])
    if action == 'TRIANGULATE':
        operation.inputs['Quad Method'].default_value = QUAD_NAMES[quad]
        operation.inputs['N-gon Method'].default_value = NGON_NAMES[ngon]
    elif action in ('SMOOTH', 'FLAT'):
        operation.domain = 'FACE'
        operation.inputs['Shade Smooth'].default_value = action == 'SMOOTH'
    selection = group.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = 'FACE'
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


def native(action, indices, quad='BEAUTY', ngon='BEAUTY'):
    obj = bpy.data.objects.new('Face Tools Native Reference', source().data.copy())
    bpy.context.collection.objects.link(obj)
    for other in bpy.context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.object.mode_set(mode='EDIT')
    select(indices)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        if action == 'TRIANGULATE':
            assert bpy.ops.mesh.quads_convert_to_tris(quad_method=quad, ngon_method=ngon) == {'FINISHED'}
        elif action == 'FLIP':
            assert bpy.ops.mesh.flip_normals() == {'FINISHED'}
        else:
            assert getattr(bpy.ops.mesh, 'faces_shade_smooth' if action == 'SMOOTH' else
                           'faces_shade_flat')() == {'FINISHED'}
        bpy.ops.object.mode_set(mode='OBJECT')
    result = snapshot(obj.data)
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    return result


CASES = [('TRIANGULATE', [0, 2], q, n) for q in QUAD_NAMES for n in NGON_NAMES]
CASES += [('TRIANGULATE', [0, 1, 2, 3], 'BEAUTY', 'BEAUTY'),
          ('FLIP', [0, 2], 'BEAUTY', 'BEAUTY'), ('SMOOTH', [0, 2], 'BEAUTY', 'BEAUTY'),
          ('FLAT', [1, 3], 'BEAUTY', 'BEAUTY')]
for action, indices, quad, ngon in CASES:
    setup()
    original = mesh_snapshot()
    expected = reference(action, indices, quad, ngon)
    # Geometry Nodes kernels can reorder domains; native parity compares winding and attributes.
    assert canonical(native(action, indices, quad, ngon)) == canonical(expected), (action, quad, ngon)
    begin()
    select(indices)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_face.poll()
        assert not bpy.ops.mesh.quads_convert_to_tris.poll()
    assert face_tool(action, quad_method=quad, ngon_method=ngon) == {'FINISHED'}, (action, quad, ngon)
    assert_retired(5)
    operation = result_node()
    assert operation.bl_idname == OP_TYPES[action]
    capture = next(l.from_node for l in tree().links if l.to_node == operation and l.to_socket.name == 'Selection')
    assert capture.domain == 'FACE' and capture.selected_count == len(indices)
    assert capture.location.x < operation.location.x < viewer().location.x
    assert evaluate_branch() == expected
    assert mesh_snapshot() == original
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.ed.undo() == {'FINISHED'}
    assert_retired(3)
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.ed.redo() == {'FINISHED'}
    assert_retired(5)
    assert evaluate_branch() == expected
    checks += 1

# No empty captures, wrong-domain edits or already-applied shading/triangulation nodes.
for action, indices, domain in [('TRIANGULATE', [], 'FACE'), ('FLIP', [], 'FACE'),
                              ('SMOOTH', [], 'FACE'), ('FLAT', [], 'FACE'),
                              ('TRIANGULATE', [3], 'FACE'), ('SMOOTH', [1], 'FACE'),
                              ('FLAT', [0], 'FACE'), ('FLIP', [0], 'POINT'), ('FLIP', [0], 'EDGE')]:
    setup()
    begin()
    select(indices, domain)
    assert face_tool(action) == {'CANCELLED'}, (action, indices, domain)
    assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
    end()
    checks += 1

# Triangulate cannot remap hidden edge indices. Flip/shading retain stable hidden indices.
setup()
begin()
select([0])
bm = bmesh.from_edit_mesh(bpy.context.object.data)
bm.faces.ensure_lookup_table()
bm.faces[3].hide_set(True)
bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
assert face_tool('TRIANGULATE') == {'CANCELLED'}
assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
assert face_tool('FLIP') == {'FINISHED'}
assert_retired(5)
checks += 1

# Mixed operations remain editable after the session ends and after saving/reloading.
setup()
begin()
select([0, 2])
assert face_tool('TRIANGULATE') == {'FINISHED'}
begin()
select([0])
assert face_tool('FLIP') == {'FINISHED'}
begin()
select([0])
assert face_tool('SMOOTH') == {'FINISHED'}
assert_retired(9)
expected = evaluate_branch()
with tempfile.TemporaryDirectory(prefix='viewer-face-tools-') as directory:
    filename = os.path.join(directory, 'faces.blend')
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.wm.save_as_mainfile(filepath=filename)
        bpy.ops.wm.open_mainfile(filepath=filename)
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert evaluate_branch() == expected
checks += 1
print('GEOMETRY_VIEWER_FACE_TOOLS_SMOKE_OK', checks)
