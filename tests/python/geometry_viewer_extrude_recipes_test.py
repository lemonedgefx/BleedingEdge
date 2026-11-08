# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Extrude Mesh/Top replay contract, runnable on the existing Phase 0 build.

This verifies existing geometry kernels, not the new Viewer adapter or E key.
Run --background --factory-startup --python-exit-code 1 --python this_file.py.
"""

import bmesh
import bpy
from mathutils import Vector


VIEW = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
OVERRIDE = dict(window=bpy.context.window, screen=bpy.context.screen, area=VIEW,
                region=next(r for r in VIEW.regions if r.type == 'WINDOW'))
VERTS = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0),
         (3, 0, 0), (4, 0, 0), (4, 1, 0), (3, 1, 0), (0, 0, 2), (1, 0, 2)]
FACES = [(0, 1, 4, 3), (1, 2, 5, 4), (6, 7, 8, 9)]
TOP = '.viewer_extrude_top_probe'


def snapshot(mesh):
    return ([v.co.copy() for v in mesh.vertices],
            [tuple(e.vertices) for e in mesh.edges],
            [tuple(f.vertices) for f in mesh.polygons])


def close(actual, expected):
    assert len(actual) == len(expected)
    for index, (a, b) in enumerate(zip(actual, expected)):
        assert (a - b).length < 1e-4, (index, a, b)


def run(domain, individual, sheared, selected_faces):
    mesh = bpy.data.meshes.new('Extrude Recipe Source')
    mesh.from_pydata(VERTS, [(10, 11)], FACES)
    materials = [bpy.data.materials.new(f'Extrude Recipe Material {i}') for i in range(3)]
    for material in materials:
        mesh.materials.append(material)
    uv = mesh.uv_layers.new(name='UVMap')
    for i, loop in enumerate(uv.data):
        loop.uv = (i * 0.07, i * 0.03)
    for i, face in enumerate(mesh.polygons):
        face.material_index = i
    original = snapshot(mesh)
    obj = bpy.data.objects.new('Extrude Recipe Source', mesh)
    bpy.context.collection.objects.link(obj)
    obj.rotation_euler = (0.2, -0.4, 0.3)
    obj.scale = (1.5, 0.7, 2)
    parent = bpy.data.objects.new('Extrude Recipe Parent', None)
    bpy.context.collection.objects.link(parent)
    if sheared:
        parent.rotation_euler = (-0.3, 0.2, 0.1)
        parent.scale = (2, 0.5, 1.25)
        obj.parent = parent
    tree = bpy.data.node_groups.new('Extrude Recipe', 'GeometryNodeTree')
    tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = tree.nodes.new('NodeGroupInput')
    output = tree.nodes.new('NodeGroupOutput')
    selection = tree.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = domain
    indices = {'POINT': [0, 10], 'EDGE': [0, 1], 'FACE': selected_faces}[domain]
    selection.inputs['Indices'].default_value = ','.join(map(str, indices))
    extrude = tree.nodes.new('GeometryNodeExtrudeMesh')
    extrude.mode = {'POINT': 'VERTICES', 'EDGE': 'EDGES', 'FACE': 'FACES'}[domain]
    if domain == 'FACE':
        extrude.inputs['Individual'].default_value = individual
    offset = tree.nodes.new('FunctionNodeInputVector')
    store = tree.nodes.new('GeometryNodeStoreNamedAttribute')
    store.data_type = 'BOOLEAN'
    store.domain = domain
    store.inputs['Name'].default_value = TOP
    tree.links.new(source.outputs['Geometry'], extrude.inputs['Mesh'])
    tree.links.new(selection.outputs['Selection'], extrude.inputs['Selection'])
    tree.links.new(offset.outputs['Vector'], extrude.inputs['Offset'])
    tree.links.new(extrude.outputs['Mesh'], store.inputs['Geometry'])
    tree.links.new(extrude.outputs['Top'], store.inputs['Value'])
    tree.links.new(store.outputs['Geometry'], output.inputs['Geometry'])
    modifier = obj.modifiers.new('Extrude Recipe', 'NODES')
    modifier.node_group = tree

    def evaluated_copy():
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        result = evaluated.to_mesh(preserve_all_data_layers=True,
                                   depsgraph=bpy.context.evaluated_depsgraph_get()).copy()
        evaluated.to_mesh_clear()
        return result

    offset.vector = (0, 0, 0)
    zero = evaluated_copy()
    baseline, edges, faces = snapshot(zero)
    top = [i for i, value in enumerate(zero.attributes[TOP].data) if value.value]
    assert top
    points = set(top) if domain == 'POINT' else set()
    collection = zero.edges if domain == 'EDGE' else zero.polygons
    if domain != 'POINT':
        for index in top:
            points.update(collection[index].vertices)
    assert len(zero.vertices) > len(VERTS) or len(zero.edges) > len(mesh.edges)
    # Original UV corners and face assignments remain present in this kernel's ordering.
    assert zero.uv_layers['UVMap'] is not None
    assert [f.material_index for f in zero.polygons[:len(FACES)]] == [0, 1, 2]
    for i, loop in enumerate(mesh.uv_layers['UVMap'].data):
        close([zero.uv_layers['UVMap'].data[i].uv], [loop.uv])
    world = obj.matrix_world.copy()
    inverse = world.inverted().to_3x3()
    count = 0
    for world_delta in ((0.2, -0.3, 0.4), (-0.1, 0.2, 0.1), (0, 0, 0),
                        (0.4, 0.1, -0.2), (0, 0, 0)):
        delta = Vector(world_delta)
        local = inverse @ delta
        offset.vector = local
        result = evaluated_copy()
        actual, result_edges, result_faces = snapshot(result)
        expected = [p + local if i in points else p for i, p in enumerate(baseline)]
        close(actual, expected)
        assert result_edges == edges and result_faces == faces
        assert [i for i, v in enumerate(result.attributes[TOP].data) if v.value] == top

        # Native translation on the authoritative zero-offset Top matches the replayed Offset.
        native_mesh = zero.copy()
        native_obj = bpy.data.objects.new('Extrude Recipe Native', native_mesh)
        bpy.context.collection.objects.link(native_obj)
        native_obj.parent = obj.parent
        native_obj.matrix_basis = obj.matrix_basis.copy()
        for other in bpy.context.selected_objects:
            other.select_set(False)
        native_obj.select_set(True)
        bpy.context.view_layer.objects.active = native_obj
        bpy.context.view_layer.update()
        with bpy.context.temp_override(**OVERRIDE):
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.mesh.select_all(action='DESELECT')
            bpy.ops.mesh.select_mode(type={'POINT': 'VERT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain])
        bm = bmesh.from_edit_mesh(native_mesh)
        for elements in (bm.verts, bm.edges, bm.faces):
            elements.ensure_lookup_table()
        picked = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
        for index in top:
            picked[index].select_set(True)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(native_mesh, loop_triangles=False, destructive=False)
        with bpy.context.temp_override(**OVERRIDE):
            assert bpy.ops.transform.translate(value=delta, orient_type='GLOBAL',
                                                use_proportional_edit=False, mirror=False) == {'FINISHED'}
        close([v.co.copy() for v in bm.verts], actual)
        del bm, picked
        with bpy.context.temp_override(**OVERRIDE):
            bpy.ops.object.mode_set(mode='OBJECT')
        bpy.data.objects.remove(native_obj, do_unlink=True)
        bpy.data.meshes.remove(native_mesh)
        bpy.data.meshes.remove(result)
        count += 1
    assert snapshot(mesh)[1:] == original[1:]
    close(snapshot(mesh)[0], original[0])
    bpy.data.meshes.remove(zero)
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.objects.remove(parent, do_unlink=True)
    bpy.data.meshes.remove(mesh)
    bpy.data.node_groups.remove(tree)
    for material in materials:
        bpy.data.materials.remove(material)
    return count


checks = 0
for sheared in (False, True):
    for domain, individual, selected in (('POINT', False, [0]), ('EDGE', False, [0]),
                                         ('FACE', False, [0, 1]), ('FACE', False, [0, 2]),
                                         ('FACE', True, [0, 1])):
        checks += run(domain, individual, sheared, selected)
print('GEOMETRY_VIEWER_EXTRUDE_RECIPES_OK', checks)
