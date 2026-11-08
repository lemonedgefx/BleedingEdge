# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare existing GN transform recipes with native mesh transforms.

This kernel-contract test does not require the new Viewer transform adapter; it
can run on the Phase 0 build. Run with --background --factory-startup and
--python-exit-code 1. It is not a test of the new modal/lifecycle integration.
"""

import bmesh
import bpy
from mathutils import Matrix, Vector


VIEW = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
OVERRIDE = dict(window=bpy.context.window, screen=bpy.context.screen, area=VIEW,
                region=next(r for r in VIEW.regions if r.type == 'WINDOW'))
POSITIONS = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0),
             (3, -1, 0), (5, -1, 0), (5, 1, 0), (3, 1, 0), (0, 0, 2)]
FACES = [(0, 1, 2, 3), (4, 5, 6, 7)]


def close(actual, expected, label):
    assert len(actual) == len(expected), label
    for index, (a, b) in enumerate(zip(actual, expected)):
        tolerance = 2e-5 * max(1, *map(abs, a), *map(abs, b))
        assert (a - b).length < 2 * tolerance, (label, index, a, b)


def fixture(sheared):
    mesh = bpy.data.meshes.new('Transform Recipe Mesh')
    mesh.from_pydata(POSITIONS, [], FACES)
    obj = bpy.data.objects.new('Transform Recipe Source', mesh)
    bpy.context.collection.objects.link(obj)
    for other in bpy.context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj.location = (0.7, -0.4, 0.2)
    obj.rotation_euler = (0.2, -0.4, 0.3)
    obj.scale = (1.5, 0.7, 2)
    parent = None
    if sheared:
        parent = bpy.data.objects.new('Transform Recipe Parent', None)
        bpy.context.collection.objects.link(parent)
        parent.rotation_euler = (-0.3, 0.2, 0.1)
        parent.scale = (2, 0.5, 1.25)
        obj.parent = parent
    bpy.context.view_layer.update()
    return obj, parent


def recipe(obj, domain, indices, point_indices, local_matrix, local_pivot, mode, factor):
    tree = bpy.data.node_groups.new('Transform Recipe Test', 'GeometryNodeTree')
    tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = tree.nodes.new('NodeGroupInput')
    output = tree.nodes.new('NodeGroupOutput')
    selection = tree.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = domain if mode in {'UNIFORM', 'SINGLE_AXIS'} else 'POINT'
    picked = indices if mode in {'UNIFORM', 'SINGLE_AXIS'} else point_indices
    selection.inputs['Indices'].default_value = ','.join(map(str, picked))
    operation = tree.nodes.new('GeometryNodeScaleElements' if mode in {'UNIFORM', 'SINGLE_AXIS'}
                               else 'GeometryNodeSetPosition')
    if mode in {'UNIFORM', 'SINGLE_AXIS'}:
        operation.domain = domain
        operation.inputs['Scale Mode'].default_value = {'UNIFORM': 'Uniform', 'SINGLE_AXIS': 'Single Axis'}[mode]
        operation.inputs['Scale'].default_value = factor
        operation.inputs['Axis'].default_value = (1, 0, 0)
        center = tree.nodes.new('FunctionNodeInputVector')
        center.vector = local_pivot
        tree.links.new(center.outputs['Vector'], operation.inputs['Center'])
    elif mode == 'MOVE':
        operation.inputs['Offset'].default_value = local_matrix.translation
    else:
        position = tree.nodes.new('GeometryNodeInputPosition')
        matrix = tree.nodes.new('FunctionNodeCombineMatrix')
        transform = tree.nodes.new('FunctionNodeTransformPoint')
        for column in range(4):
            for row in range(4):
                matrix.inputs[f'Column {column + 1} Row {row + 1}'].default_value = local_matrix[row][column]
        tree.links.new(position.outputs['Position'], transform.inputs['Vector'])
        tree.links.new(matrix.outputs['Matrix'], transform.inputs['Transform'])
        tree.links.new(transform.outputs['Vector'], operation.inputs['Position'])
    tree.links.new(source.outputs['Geometry'], operation.inputs['Geometry'])
    tree.links.new(selection.outputs['Selection'], operation.inputs['Selection'])
    tree.links.new(operation.outputs['Geometry'], output.inputs['Geometry'])
    modifier = obj.modifiers.new('Transform Recipe Test', 'NODES')
    modifier.node_group = tree
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    result = evaluated.to_mesh()
    positions = [v.co.copy() for v in result.vertices]
    assert [tuple(f.vertices) for f in result.polygons] == FACES
    evaluated.to_mesh_clear()
    obj.modifiers.remove(modifier)
    bpy.data.node_groups.remove(tree)
    return positions


def check(domain, mode, sheared=False, disconnected=False):
    obj, parent = fixture(sheared)
    world = obj.matrix_world.copy()
    with bpy.context.temp_override(**OVERRIDE):
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type={'POINT': 'VERT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain])
    bm = bmesh.from_edit_mesh(obj.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    elements = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    indices = [0, 1] if disconnected and domain == 'FACE' else [0]
    if domain == 'POINT':
        indices = [0, 1, 8]
    for index in indices:
        elements[index].select_set(True)
        bm.select_history.add(elements[index])
    bm.select_flush_mode()
    point_indices = [v.index for v in bm.verts if v.select]
    local_pivot = Vector((0.3, -0.2, 0.4))
    pivot = world @ local_pivot
    bpy.context.scene.tool_settings.transform_pivot_point = 'CURSOR'
    bpy.context.scene.cursor.location = pivot
    bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)
    factor = 1.7
    angle = 0.63
    delta = Vector((0.2, -0.3, 0.4))
    with bpy.context.temp_override(**OVERRIDE):
        common = dict(use_proportional_edit=False, mirror=False)
        if mode == 'MOVE':
            assert bpy.ops.transform.translate(value=delta, orient_type='GLOBAL', **common) == {'FINISHED'}
            transform = Matrix.Translation(delta)
        elif mode == 'ROTATE':
            assert bpy.ops.transform.rotate(value=angle, orient_axis='Z', orient_type='GLOBAL',
                                            constraint_axis=(False, False, True), **common) == {'FINISHED'}
            linear = Matrix.Rotation(angle, 4, 'Z')
            transform = Matrix.Translation(pivot) @ linear @ Matrix.Translation(-pivot)
        else:
            values = ((factor,) * 3 if mode == 'UNIFORM' else
                      (-factor, factor, factor) if mode == 'REFLECT' else
                      (factor, 0.8, 1.2) if mode == 'XYZ' else (factor, 1, 1))
            orientation = 'LOCAL' if mode == 'SINGLE_AXIS' else 'GLOBAL'
            constraint = (True, False, False) if mode == 'SINGLE_AXIS' else (False,) * 3
            assert bpy.ops.transform.resize(value=values, orient_type=orientation,
                                            constraint_axis=constraint, **common) == {'FINISHED'}
            linear = Matrix.Diagonal((*values, 1))
            if mode == 'SINGLE_AXIS':
                orientation_matrix = world.to_3x3().normalized().to_4x4()
                linear = orientation_matrix @ linear @ orientation_matrix.inverted()
            transform = Matrix.Translation(pivot) @ linear @ Matrix.Translation(-pivot)
    native = [v.co.copy() for v in bm.verts]
    del bm, elements
    with bpy.context.temp_override(**OVERRIDE):
        bpy.ops.object.mode_set(mode='OBJECT')
    for vertex, position in zip(obj.data.vertices, POSITIONS):
        vertex.co = position
    obj.data.update()
    local = world.inverted() @ transform @ world
    expected = [local @ Vector(p) if i in point_indices else Vector(p) for i, p in enumerate(POSITIONS)]
    close(native, expected, ('native matrix', domain, mode, sheared, disconnected))
    # Point scale always uses the affine recipe; global-axis/sheared and reflection do too.
    node_mode = ('MATRIX' if domain == 'POINT' and mode != 'MOVE' else
                 'MATRIX' if mode in {'ROTATE', 'XYZ', 'REFLECT', 'WORLD_AXIS'} else mode)
    result = recipe(obj, domain, indices, point_indices, local, local_pivot, node_mode, factor)
    close(result, native, ('node recipe', domain, mode, sheared, disconnected))
    bpy.data.objects.remove(obj, do_unlink=True)
    if parent:
        bpy.data.objects.remove(parent, do_unlink=True)


cases = 0
for domain in ('POINT', 'EDGE', 'FACE'):
    for mode in ('MOVE', 'UNIFORM', 'ROTATE', 'XYZ', 'REFLECT', 'WORLD_AXIS'):
        for sheared in (False, True):
            check(domain, mode, sheared)
            cases += 1
for domain in ('EDGE', 'FACE'):
    check(domain, 'SINGLE_AXIS')
    cases += 1
check('FACE', 'UNIFORM', disconnected=True)
cases += 1
print('GEOMETRY_VIEWER_TRANSFORM_RECIPES_OK', cases, flush=True)
