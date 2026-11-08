# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Connected Viewer duplication recipes compared with ordinary Edit Mode duplication.

This tests existing node recipes, so it can run before the Viewer adapter is compiled.
Run --background --factory-startup --python-exit-code 1 --python this_file.py.
"""

import bmesh
import bpy
from mathutils import Vector


checks = 0


def mesh_new():
    mesh = bpy.data.meshes.new('Connected Duplication Fixture')
    mesh.from_pydata([(x, y, 0) for y in range(2) for x in range(4)] +
                     [(5, 0, 0), (6, 0, 0), (8, 0, 0)], [(8, 9)],
                     [(x, x + 1, x + 5, x + 4) for x in range(3)])
    for domain, name in (('POINT', 'source_point'), ('EDGE', 'source_edge'), ('FACE', 'source_face')):
        attr = mesh.attributes.new(name, 'INT', domain)
        for i, value in enumerate(attr.data):
            value.value = i + 100
    uv = mesh.uv_layers.new(name='UVMap')
    for i, value in enumerate(uv.data):
        value.uv = (i * 0.0625, i * 0.125 + 0.25)
    for _ in range(2):
        mesh.materials.append(bpy.data.materials.new('Duplicate Material'))
    for face in mesh.polygons:
        face.material_index = face.index % 2
    return mesh


def obj_new(mesh):
    obj = bpy.data.objects.new('Connected Duplication', mesh)
    bpy.context.collection.objects.link(obj)
    for other in bpy.context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj.rotation_euler, obj.scale = (0.2, -0.4, 0.3), (1.5, 0.7, 2)
    bpy.context.view_layer.update()
    return obj


def snapshot(mesh):
    def co(index):
        return tuple(round(v, 5) for v in mesh.vertices[index].co)

    def cyclic(values):
        return min(tuple(values[i:] + values[:i]) for i in range(len(values)))

    return (
        sorted((co(v.index), mesh.attributes['source_point'].data[v.index].value) for v in mesh.vertices),
        sorted((tuple(sorted(co(i) for i in edge.vertices)),
                mesh.attributes['source_edge'].data[edge.index].value) for edge in mesh.edges),
        sorted((cyclic([(co(mesh.loops[i].vertex_index),
                         tuple(round(v, 5) for v in mesh.uv_layers['UVMap'].data[i].uv))
                        for i in face.loop_indices]),
                mesh.attributes['source_face'].data[face.index].value, face.material_index)
               for face in mesh.polygons),
    )


def native(mesh, domain, indices, offset):
    obj = obj_new(mesh.copy())
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.mesh.select_mode(type={'POINT': 'VERT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain])
    bm = bmesh.from_edit_mesh(obj.data)
    elements = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    elements.ensure_lookup_table()
    for index in indices:
        elements[index].select_set(True)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)
    assert bpy.ops.mesh.duplicate() == {'FINISHED'}
    assert bpy.ops.transform.translate(value=obj.matrix_world.to_3x3() @ Vector(offset)) == {'FINISHED'}
    bpy.ops.object.mode_set(mode='OBJECT')
    result = snapshot(obj.data)
    bpy.data.objects.remove(obj, do_unlink=True)
    return result


def recipe(mesh, domain, indices, offset, picking=False):
    obj = obj_new(mesh.copy())
    group = bpy.data.node_groups.new('Connected Duplicate Recipe', 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = group.nodes.new('NodeGroupInput')
    output = group.nodes.new('NodeGroupOutput')
    selection = group.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = domain
    selection.inputs['Indices'].default_value = ', '.join(map(str, indices))
    separate = group.nodes.new('GeometryNodeSeparateGeometry')
    separate.domain = domain
    move = group.nodes.new('GeometryNodeSetPosition')
    move.inputs['Offset'].default_value = offset
    join = group.nodes.new('GeometryNodeJoinGeometry')
    group.links.new(source.outputs['Geometry'], separate.inputs['Geometry'])
    group.links.new(selection.outputs['Selection'], separate.inputs['Selection'])
    group.links.new(separate.outputs['Selection'], move.inputs['Geometry'])
    branch = move
    if picking:
        branch = group.nodes.new('GeometryNodeStoreNamedAttribute')
        branch.domain, branch.data_type = domain, 'BOOLEAN'
        branch.inputs['Name'].default_value = '.viewer_duplicate_test'
        branch.inputs['Value'].default_value = True
        group.links.new(move.outputs['Geometry'], branch.inputs['Geometry'])
    group.links.new(branch.outputs['Geometry'], join.inputs['Geometry'])
    group.links.new(source.outputs['Geometry'], join.inputs['Geometry'])
    group.links.new(join.outputs['Geometry'], output.inputs['Geometry'])
    obj.modifiers.new('Connected Duplicate Recipe', 'NODES').node_group = group
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    result_mesh = evaluated.to_mesh(preserve_all_data_layers=True,
                                    depsgraph=bpy.context.evaluated_depsgraph_get())
    result = snapshot(result_mesh)
    ordered = (tuple(tuple(v.co) for v in result_mesh.vertices),
               tuple(tuple(e.vertices) for e in result_mesh.edges),
               tuple(tuple(f.vertices) for f in result_mesh.polygons))
    if picking:
        attr = result_mesh.attributes['.viewer_duplicate_test']
        picked = [i for i, v in enumerate(attr.data) if v.value]
        assert len(picked) == len(indices), (domain, indices, picked)
        original_size = {'POINT': len(mesh.vertices), 'EDGE': len(mesh.edges), 'FACE': len(mesh.polygons)}[domain]
        assert all(i >= original_size for i in picked), (domain, picked)
    evaluated.to_mesh_clear()
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.node_groups.remove(group)
    return result, ordered


mesh = mesh_new()
shared_edge = next(e.index for e in mesh.edges if set(e.vertices) == {1, 5})
loose_edge = next(e.index for e in mesh.edges if set(e.vertices) == {8, 9})
cases = (
    ('POINT', [0]), ('POINT', [8, 9, 10]), ('POINT', [0, 1, 4, 5]),
    ('POINT', list(range(11))), ('EDGE', [shared_edge]),
    ('EDGE', [shared_edge, loose_edge]), ('EDGE', [e.index for e in mesh.edges]),
    ('FACE', [0]), ('FACE', [0, 1]), ('FACE', [0, 2]), ('FACE', [0, 1, 2]),
)
for domain, indices in cases:
    for offset in ((0, 0, 0), (0.25, -0.5, 1.25)):
        expected = native(mesh, domain, indices, offset)
        actual, ordered = recipe(mesh, domain, indices, offset)
        assert actual == expected, (domain, indices, offset)
        tagged, tagged_order = recipe(mesh, domain, indices, offset, picking=True)
        assert tagged == actual and tagged_order == ordered, (domain, 'Temporary picking must not reorder geometry')
        checks += 2
print('GEOMETRY_VIEWER_DUPLICATE_RECIPES_OK', checks)
