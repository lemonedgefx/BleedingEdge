# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Existing Loop Cut kernel contract, not the new Viewer Ctrl+R adapter.

Can run on the existing Phase 0 executable. Checks oriented preview geometry,
Loops/Loop Index, attribute interpolation, count changes and rejected rings.
Run --background --factory-startup --python-exit-code 1 --python this_file.py.
"""

from collections import deque
from math import cos, sin, tau

import bpy
from mathutils import Vector


def ring(mesh, seed):
    """Independent quad-opposite reference using stored seed endpoint order."""
    edges = [tuple(e.vertices) for e in mesh.edges]
    lookup = {tuple(sorted(e)): i for i, e in enumerate(edges)}
    adjacent = [[] for _ in edges]
    for face in mesh.polygons:
        for key in face.edge_keys:
            adjacent[lookup[tuple(sorted(key))]].append(face.index)
    orientation = {seed: edges[seed]}
    faces = set()
    queue = deque([seed])
    valid = True
    while queue:
        edge = queue.popleft()
        valid &= len(adjacent[edge]) <= 2
        for face_index in adjacent[edge]:
            verts = list(mesh.polygons[face_index].vertices)
            if len(verts) != 4:
                continue
            faces.add(face_index)
            for corner in range(4):
                forward = (verts[corner], verts[(corner + 1) % 4])
                if set(forward) == set(edges[edge]):
                    opposite = (verts[(corner + 3) % 4], verts[(corner + 2) % 4])
                    if orientation[edge] != forward:
                        opposite = opposite[::-1]
                    other = lookup[tuple(sorted(opposite))]
                    if other in orientation:
                        valid &= orientation[other] == opposite
                    else:
                        orientation[other] = opposite
                        queue.append(other)
                    break
    return (orientation, sorted(faces)) if valid and faces else ({}, [])


def expected_lines(mesh, seed, cuts, factor):
    orientation, faces = ring(mesh, seed)
    lookup = {tuple(sorted(e.vertices)): e.index for e in mesh.edges}
    result = []
    for face_index in faces:
        face = mesh.polygons[face_index]
        keys = [lookup[tuple(sorted(k))] for k in face.edge_keys]
        first = next(i for i, edge in enumerate(keys) if edge in orientation)
        a = orientation[keys[first]]
        b = orientation[keys[(first + 2) % 4]]
        for cut in range(cuts):
            t = (cut + 1 + factor) / (cuts + 1)
            result.append((cut, mesh.vertices[a[0]].co.lerp(mesh.vertices[a[1]].co, t),
                           mesh.vertices[b[0]].co.lerp(mesh.vertices[b[1]].co, t)))
    return result


def topology(mesh):
    return ([tuple(e.vertices) for e in mesh.edges], [tuple(f.vertices) for f in mesh.polygons])


def fixture(name, vertices, faces, explicit_edges=()):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, explicit_edges, faces)
    original = ([v.co.copy() for v in mesh.vertices], topology(mesh))
    weight = mesh.attributes.new('weight', 'FLOAT', 'POINT')
    for v, value in zip(mesh.vertices, weight.data):
        value.value = v.co.x + 2 * v.co.y + 3 * v.co.z
    source_face = mesh.attributes.new('source_face', 'INT', 'FACE')
    for face, value in zip(mesh.polygons, source_face.data):
        value.value = face.index
    uv = mesh.uv_layers.new(name='UVMap')
    for face in mesh.polygons:
        for corner in face.loop_indices:
            co = mesh.vertices[mesh.loops[corner].vertex_index].co
            uv.data[corner].uv = (co.x + 10 * face.index, co.y + 20 * face.index)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    tree = bpy.data.node_groups.new(name, 'GeometryNodeTree')
    tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = tree.nodes.new('NodeGroupInput')
    output = tree.nodes.new('NodeGroupOutput')
    selection = tree.nodes.new('GeometryNodeSelectByIndex')
    selection.domain = 'EDGE'
    cut = tree.nodes.new('GeometryNodeLoopCutSlide')
    loops = tree.nodes.new('GeometryNodeStoreNamedAttribute')
    loops.data_type, loops.domain = 'BOOLEAN', 'EDGE'
    loops.inputs['Name'].default_value = 'test_loops'
    indices = tree.nodes.new('GeometryNodeStoreNamedAttribute')
    indices.data_type, indices.domain = 'INT', 'EDGE'
    indices.inputs['Name'].default_value = 'test_loop_index'
    tree.links.new(source.outputs['Geometry'], cut.inputs['Mesh'])
    tree.links.new(selection.outputs['Selection'], cut.inputs['Selection'])
    tree.links.new(cut.outputs['Mesh'], loops.inputs['Geometry'])
    tree.links.new(cut.outputs['Loops'], loops.inputs['Value'])
    tree.links.new(loops.outputs['Geometry'], indices.inputs['Geometry'])
    tree.links.new(cut.outputs['Loop Index'], indices.inputs['Value'])
    tree.links.new(indices.outputs['Geometry'], output.inputs['Geometry'])
    obj.modifiers.new(name, 'NODES').node_group = tree
    checks = 0
    # Test every seed direction, including boundary and closed rings.
    for seed in range(len(mesh.edges)):
        selection.inputs['Indices'].default_value = str(seed)
        fixed_topology = None
        fixed_selection = None
        for cuts in (1, 3, 2, 1):
            cut.inputs['Cuts'].default_value = cuts
            for factor in (0, -0.7, 0.65, -1, 1, 0):
                cut.inputs['Factor'].default_value = factor
                bpy.context.view_layer.update()
                evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                result = evaluated.to_mesh(preserve_all_data_layers=True,
                                          depsgraph=bpy.context.evaluated_depsgraph_get())
                actual_indices = [v.value for v in result.attributes['test_loop_index'].data]
                selected = [i for i, v in enumerate(result.attributes['test_loops'].data) if v.value]
                expected = expected_lines(mesh, seed, cuts, factor)
                assert len(selected) == len(expected), (name, seed, cuts, factor, len(selected), len(expected))
                assert all((index >= 0) == (i in selected) for i, index in enumerate(actual_indices))
                remaining = set(selected)
                for index, a, b in expected:
                    found = next((e for e in remaining if actual_indices[e] == index and
                                  ((result.vertices[result.edges[e].vertices[0]].co - a).length < 1e-4 and
                                   (result.vertices[result.edges[e].vertices[1]].co - b).length < 1e-4 or
                                   (result.vertices[result.edges[e].vertices[1]].co - a).length < 1e-4 and
                                   (result.vertices[result.edges[e].vertices[0]].co - b).length < 1e-4)), None)
                    assert found is not None, (name, seed, cuts, factor, index, a, b)
                    remaining.remove(found)
                if expected:
                    assert set(actual_indices) - {-1} == set(range(cuts))
                    if fixed_topology and fixed_topology[0] == cuts:
                        assert topology(result) == fixed_topology[1]
                        assert selected == fixed_selection
                    fixed_topology = cuts, topology(result)
                    fixed_selection = selected
                else:
                    assert topology(result) == original[1]
                    assert all(i == -1 for i in actual_indices)
                for v, value in zip(result.vertices, result.attributes['weight'].data):
                    assert abs(value.value - (v.co.x + 2 * v.co.y + 3 * v.co.z)) < 1e-4
                for face in result.polygons:
                    source_id = result.attributes['source_face'].data[face.index].value
                    for corner in face.loop_indices:
                        co = result.vertices[result.loops[corner].vertex_index].co
                        actual_uv = result.uv_layers['UVMap'].data[corner].uv
                        assert (actual_uv - Vector((co.x + 10 * source_id, co.y + 20 * source_id))).length < 1e-4
                evaluated.to_mesh_clear()
                checks += 1
    # Empty selection always emits the documented fallback fields.
    selection.inputs['Indices'].default_value = ''
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    result = evaluated.to_mesh(preserve_all_data_layers=True,
                              depsgraph=bpy.context.evaluated_depsgraph_get())
    assert topology(result) == original[1]
    assert not any(v.value for v in result.attributes['test_loops'].data)
    assert all(v.value == -1 for v in result.attributes['test_loop_index'].data)
    evaluated.to_mesh_clear()
    assert topology(mesh) == original[1]
    assert all((v.co - p).length == 0 for v, p in zip(mesh.vertices, original[0]))
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.meshes.remove(mesh)
    bpy.data.node_groups.remove(tree)
    return checks + 1


checks = fixture('Quad Strip', [(x, y, 0.1 * x * y) for y in range(2) for x in range(4)],
                 [(x, x + 1, x + 5, x + 4) for x in range(3)])
checks += fixture('Closed Cylinder', [(cos(tau * i / 6), sin(tau * i / 6), z)
                                     for z in range(2) for i in range(6)],
                  [(i, (i + 1) % 6, (i + 1) % 6 + 6, i + 6) for i in range(6)])
checks += fixture('Triangle', [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)])
checks += fixture('Wire', [(0, 0, 0), (1, 0, 0)], [], [(0, 1)])
checks += fixture('Non Manifold', [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                                   (0, -1, 0), (1, -1, 0), (1, 0, 1), (0, 0, 1)],
                  [(0, 1, 2, 3), (1, 0, 4, 5), (0, 1, 6, 7)])
print('GEOMETRY_VIEWER_LOOP_CUT_RECIPES_OK', checks)
