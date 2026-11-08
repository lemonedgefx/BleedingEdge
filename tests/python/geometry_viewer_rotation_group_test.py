# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Editable Set Rotation group: protected capture, world-space parity and persistence.

Default mode constructs the proposed ordinary-node recipe around an actual captured selection
using the existing executable. It does NOT exercise the new C++ grouping adapter.
Add -- --adapter with a rebuilt executable to verify automatic rotation grouping instead.
"""

import math
import os
import sys
import tempfile

import bmesh
import bpy
from mathutils import Euler, Matrix, Vector


ADAPTER = '--adapter' in sys.argv
NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
POSITIONS = [(-1, -1, 0), (1, -1, 0), (1, 1, 0.2), (-1, 1, 0),
             (3, -1, 0), (5, -1, 0), (5, 1, 0), (3, 1, 0), (0, 0, 2)]
FACES = [(0, 1, 2, 3), (4, 5, 6, 7)]
checks = 0


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def close(actual, expected):
    assert len(actual) == len(expected)
    for a, b in zip(actual, expected):
        assert (Vector(a) - Vector(b)).length < 8e-5, (a, b)


def snapshot(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    try:
        return ([v.co.copy() for v in mesh.vertices],
                [tuple(e.vertices) for e in mesh.edges],
                [tuple(f.vertices) for f in mesh.polygons],
                [d.value for d in mesh.attributes['source_face'].data],
                [tuple(d.uv) for d in mesh.uv_layers[0].data])
    finally:
        evaluated.to_mesh_clear()


def make_fixture(sheared):
    mesh = bpy.data.meshes.new('Set Rotation Recipe Mesh')
    mesh.from_pydata(POSITIONS, [], FACES)
    ids = mesh.attributes.new('id', 'INT', 'POINT')
    for i, value in enumerate(ids.data):
        value.value = 100 + i
    faces = mesh.attributes.new('source_face', 'INT', 'FACE')
    for i, value in enumerate(faces.data):
        value.value = 20 + i
    uv = mesh.uv_layers.new(name='UVMap')
    for i, value in enumerate(uv.data):
        value.uv = (i * 0.13, i * 0.07 + 0.2)
    obj = bpy.data.objects.new('Set Rotation Recipe Source', mesh)
    bpy.context.collection.objects.link(obj)
    for other in bpy.context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj.location, obj.rotation_euler, obj.scale = (0.7, -0.4, 0.2), (0.2, -0.4, 0.3), (1.5, 0.7, 2)
    if sheared:
        parent = bpy.data.objects.new('Set Rotation Recipe Parent', None)
        bpy.context.collection.objects.link(parent)
        parent.rotation_euler, parent.scale = (-0.3, 0.2, 0.1), (2, 0.5, 1.25)
        obj.parent = parent
    group = bpy.data.node_groups.new('Rotation Recipe Parent Tree', 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source, output, viewer = (group.nodes.new(t) for t in
                              ('NodeGroupInput', 'NodeGroupOutput', 'GeometryNodeViewer'))
    viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
    group.links.new(source.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(source.outputs['Geometry'], viewer.inputs['Geometry'])
    source.location, viewer.location = (0, 80), (500, 80)
    obj.modifiers.new('Set Rotation Recipe', 'NODES').node_group = group
    NODE_AREA.type = 'DOPESHEET_EDITOR'
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    group.nodes.active = viewer
    bpy.context.view_layer.update()
    return obj, group, source, output, viewer


def capture_and_rotate(obj, parent, viewer, domain, pivot_mode):
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    element = dict(VERT=bm.verts, EDGE=bm.edges, FACE=bm.faces)[domain][0]
    element.select_set(True)
    if domain == 'VERT':
        bm.verts[1].select_set(True)
    bm.select_history.add(element)
    if domain == 'FACE':
        bm.faces.active = element
    bm.select_flush_mode()
    picked = [v.index for v in bm.verts if v.select]
    median = sum((Vector(POSITIONS[i]) for i in picked), Vector()) / len(picked)
    active = (Vector(POSITIONS[0]) if domain == 'VERT' else
              sum((v.co.copy() for v in element.verts), Vector()) / len(element.verts))
    bm = element = None
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
    bpy.context.scene.tool_settings.transform_pivot_point = ('MEDIAN_POINT' if pivot_mode == 'OVERRIDE'
                                                            else pivot_mode)
    cursor = Vector((0.25, -0.75, 1.25))
    bpy.context.scene.cursor.location = cursor
    options = dict(use_proportional_edit=False, mirror=False)
    if pivot_mode == 'OVERRIDE':
        options['center_override'] = (0.3, -0.6, 0.8)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        status = bpy.ops.transform.rotate(value=0.63, orient_type='GLOBAL', orient_axis='Z',
                                          constraint_axis=(False, False, True), **options)
        assert status == {'FINISHED'}
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE')
        if bpy.context.mode == 'EDIT_MESH':
            parent.nodes.active = viewer
            assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    world = obj.matrix_world.copy()
    pivot = (median if pivot_mode == 'MEDIAN_POINT' else active if pivot_mode == 'ACTIVE_ELEMENT' else
             world.inverted() @ (Vector(options['center_override']) if pivot_mode == 'OVERRIDE' else cursor))
    return picked, pivot


def prototype_group(parent, viewer, obj):
    """Copy real structured capture data using Blender's ordinary group operator."""
    operation = next(l.from_node for l in parent.links if l.to_node == viewer)
    if operation.bl_idname == 'GeometryNodeGroup' and operation.label == 'Set Rotation':
        return operation
    capture = next(l.from_node for l in parent.links if l.to_node == operation and l.to_socket.name == 'Selection')
    for node in parent.nodes:
        node.select = node.bl_idname not in ('NodeGroupInput', 'NodeGroupOutput', 'GeometryNodeViewer')
    parent.nodes.active = operation
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.group_make() == {'FINISHED'}
        assert bpy.ops.node.group_edit(exit=True) == {'FINISHED'}
    instance = next(l.from_node for l in parent.links if l.to_node == viewer)
    inner = instance.node_tree
    inner.name, instance.label = 'Set Rotation', 'Set Rotation'
    for socket in inner.interface.items_tree:
        if socket.item_type == 'SOCKET' and socket.socket_type == 'NodeSocketGeometry':
            socket.name = 'Mesh'
    control = inner.interface.new_socket(name='Rotation', in_out='INPUT', socket_type='NodeSocketRotation')
    control.default_value = (0, 0, 0.63)
    instance.inputs['Rotation'].default_value = control.default_value
    input_node = next(n for n in inner.nodes if n.bl_idname == 'NodeGroupInput')
    output = next(n for n in inner.nodes if n.bl_idname == 'NodeGroupOutput')
    operation = next(n for n in inner.nodes if n.bl_idname == 'GeometryNodeSetPosition')
    capture = next(n for n in inner.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
    for node in list(inner.nodes):
        if node not in (input_node, output, operation, capture):
            inner.nodes.remove(node)
    operation.inputs['Offset'].default_value = (0, 0, 0)
    position = inner.nodes.new('GeometryNodeInputPosition')
    subtract = inner.nodes.new('ShaderNodeVectorMath')
    subtract.operation = 'SUBTRACT'
    rotation = inner.nodes.new('FunctionNodeCombineTransform')
    basis = inner.nodes.new('FunctionNodeCombineMatrix')
    inverse = inner.nodes.new('FunctionNodeInvertMatrix')
    rotate_basis = inner.nodes.new('FunctionNodeMatrixMultiply')
    local = inner.nodes.new('FunctionNodeMatrixMultiply')
    transform = inner.nodes.new('FunctionNodeTransformPoint')
    add = inner.nodes.new('ShaderNodeVectorMath')
    add.operation = 'ADD'
    matrix = obj.matrix_world.to_3x3().to_4x4()
    for column in range(4):
        for row in range(4):
            basis.inputs[f'Column {column + 1} Row {row + 1}'].default_value = matrix[row][column]
    link = inner.links.new
    link(input_node.outputs['Rotation'], rotation.inputs['Rotation'])
    link(position.outputs['Position'], subtract.inputs[0])
    link(capture.outputs['Pivot'], subtract.inputs[1])
    link(basis.outputs['Matrix'], inverse.inputs['Matrix'])
    link(rotation.outputs['Transform'], rotate_basis.inputs[0])
    link(basis.outputs['Matrix'], rotate_basis.inputs[1])
    link(inverse.outputs['Matrix'], local.inputs[0])
    link(rotate_basis.outputs['Matrix'], local.inputs[1])
    link(subtract.outputs['Vector'], transform.inputs['Vector'])
    link(local.outputs['Matrix'], transform.inputs['Transform'])
    link(transform.outputs['Vector'], add.inputs[0])
    link(capture.outputs['Pivot'], add.inputs[1])
    link(add.outputs['Vector'], operation.inputs['Position'])
    assert output.inputs['Mesh'].is_linked
    return instance


def expected_positions(obj, picked, pivot, angles, positions):
    basis = obj.matrix_world.to_3x3()
    linear = basis.inverted() @ Euler(angles, 'XYZ').to_matrix() @ basis
    return [pivot + linear @ (Vector(p) - pivot) if i in picked else Vector(p)
            for i, p in enumerate(positions)]


for sheared in (False, True):
    for domain in ('VERT', 'EDGE', 'FACE'):
        for pivot_mode in ('MEDIAN_POINT', 'ACTIVE_ELEMENT', 'CURSOR', 'OVERRIDE'):
            obj, parent, source, output, viewer = make_fixture(sheared)
            original = snapshot(obj)
            picked, pivot = capture_and_rotate(obj, parent, viewer, domain, pivot_mode)
            instance = (next(l.from_node for l in parent.links if l.to_node == viewer) if ADAPTER else
                        prototype_group(parent, viewer, obj))
            assert instance.bl_idname == 'GeometryNodeGroup' and instance.label == 'Set Rotation'
            assert instance.node_tree.name.startswith('Set Rotation')
            assert [s.name for s in instance.inputs] == ['Mesh', 'Rotation']
            assert [s.name for s in instance.outputs] == ['Mesh']
            assert len(parent.nodes) == 4
            captures = [n for n in instance.node_tree.nodes if n.bl_idname == 'GeometryNodeSelectByIndex']
            assert len(captures) == 1 and captures[0].mode == 'CAPTURED'
            assert captures[0].domain == dict(VERT='POINT', EDGE='EDGE', FACE='FACE')[domain]
            if ADAPTER:
                assert captures[0].bl_rna.name == 'Select Elements'
            assert instance.location_absolute.x < viewer.location_absolute.x
            parent.links.new(instance.outputs['Mesh'], output.inputs['Geometry'])
            result = snapshot(obj)
            close(result[0], expected_positions(obj, picked, pivot, (0, 0, 0.63), POSITIONS))
            assert result[1:] == original[1:]
            angles = (0.15, -0.25, 1.1)
            instance.inputs['Rotation'].default_value = angles
            result = snapshot(obj)
            close(result[0], expected_positions(obj, picked, pivot, angles, POSITIONS))
            checks += 2
            delta = Vector((0.45, -0.3, 0.7))
            for vertex in obj.data.vertices:
                vertex.co += delta
            positions = [Vector(p) + delta for p in POSITIONS]
            updated_pivot = pivot + delta if pivot_mode in ('MEDIAN_POINT', 'ACTIVE_ELEMENT') else pivot
            close(snapshot(obj)[0], expected_positions(obj, picked, updated_pivot, angles, positions))
            checks += 1
            copied = instance.node_tree.copy()
            inner = instance.node_tree
            instance.node_tree = copied
            close(snapshot(obj)[0], expected_positions(obj, picked, updated_pivot, angles, positions))
            instance.node_tree = inner
            bpy.data.node_groups.remove(copied)
            checks += 1
            if sheared and domain == 'FACE' and pivot_mode == 'ACTIVE_ELEMENT':
                obj_name, tree_name, node_name = obj.name, parent.name, instance.name
                with tempfile.TemporaryDirectory(prefix='viewer-rotation-group-') as directory:
                    filename = os.path.join(directory, 'rotation.blend')
                    bpy.ops.wm.save_as_mainfile(filepath=filename)
                    bpy.ops.wm.open_mainfile(filepath=filename)
                    NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'NODE_EDITOR')
                    VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
                    obj, parent = bpy.data.objects[obj_name], bpy.data.node_groups[tree_name]
                    instance = parent.nodes[node_name]
                    close(snapshot(obj)[0], expected_positions(obj, picked, updated_pivot, angles, positions))
                checks += 1
            ids = obj.data.attributes['id'].data
            ids[0].value, ids[1].value = ids[1].value, ids[0].value
            close(snapshot(obj)[0], positions)  # Same-count ID reorder invalidates the hidden capture.
            checks += 1
            obj.modifiers.clear()
            bpy.data.objects.remove(obj, do_unlink=True)
            bpy.data.node_groups.remove(parent)

print('GEOMETRY_VIEWER_ROTATION_GROUP_ADAPTER_OK' if ADAPTER else
      'GEOMETRY_VIEWER_ROTATION_GROUP_RECIPE_OK', checks)
