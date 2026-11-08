# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Point/Edge/Face affected-point masks and all three existing merge modes.

Uses existing nodes, not the new Viewer operator. Can run without rebuilding.
Run --background --factory-startup --python-exit-code 1 --python this_file.py.
"""

import bpy


mesh = bpy.data.meshes.new('Viewer Merge Recipe')
mesh.from_pydata([(0, 0, 0), (0.0004, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0),
                 (0.0006, 0, 0), (4, 0, 0)], [(6, 7)], [(0, 1, 4, 3), (1, 2, 5, 4)])
for domain, name in (('POINT', 'source_point'), ('EDGE', 'source_edge'), ('FACE', 'source_face')):
    attribute = mesh.attributes.new(name, 'FLOAT', domain)
    for i, value in enumerate(attribute.data):
        value.value = i * 1.25 + 0.125
obj = bpy.data.objects.new('Viewer Merge Recipe', mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
group = bpy.data.node_groups.new('Viewer Merge Recipe', 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
source, output = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
selection = group.nodes.new('GeometryNodeSelectByIndex')
mask = group.nodes.new('GeometryNodeStoreNamedAttribute')
mask.domain, mask.data_type = 'POINT', 'BOOLEAN'
mask.inputs['Name'].default_value = 'affected_points'
merge = group.nodes.new('GeometryNodeMergeByDistance')
merge.inputs['Distance'].default_value = 0.001
group.links.new(source.outputs['Geometry'], mask.inputs['Geometry'])
group.links.new(source.outputs['Geometry'], merge.inputs['Geometry'])
obj.modifiers.new('Viewer Merge Recipe', 'NODES').node_group = group
checks = 0


def evaluate():
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    result = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    snapshot = (tuple(tuple(round(c, 5) for c in v.co) for v in result.vertices),
                tuple(tuple(e.vertices) for e in result.edges), tuple(tuple(f.vertices) for f in result.polygons),
                tuple((name, tuple(round(v.value, 5) for v in result.attributes[name].data))
                      for name in ('source_point', 'source_edge', 'source_face')))
    selected = ([i for i, v in enumerate(result.attributes['affected_points'].data) if v.value]
                if 'affected_points' in result.attributes else None)
    evaluated.to_mesh_clear()
    return snapshot, selected


edge_indices = [e.index for e in mesh.edges if set(e.vertices) in ({0, 1}, {6, 7})]
for domain, indices, points in (('POINT', [0, 1, 6], [0, 1, 6]),
                               ('EDGE', edge_indices, [0, 1, 6, 7]), ('FACE', [0], [0, 1, 3, 4])):
    selection.domain = domain
    selection.inputs['Indices'].default_value = ', '.join(map(str, indices))
    group.links.new(selection.outputs['Points'], mask.inputs['Value'])
    group.links.new(mask.outputs['Geometry'], output.inputs['Geometry'])
    assert evaluate()[1] == points, (domain, 'Affected-point expansion')
    checks += 1
    for mode in ('All', 'Connected', 'Not Connected'):
        merge.inputs['Mode'].default_value = mode
        group.links.new(selection.outputs['Points'], merge.inputs['Selection'])
        group.links.new(merge.outputs['Geometry'], output.inputs['Geometry'])
        actual = evaluate()[0]
        direct = group.nodes.new('GeometryNodeSelectByIndex')
        direct.domain = 'POINT'
        direct.inputs['Indices'].default_value = ', '.join(map(str, points))
        group.links.new(direct.outputs['Selection'], merge.inputs['Selection'])
        expected = evaluate()[0]
        group.nodes.remove(direct)
        assert actual == expected, (domain, mode)
        if domain == 'POINT':
            assert len(actual[0]) == (6 if mode == 'All' else 7), (mode, len(actual[0]))
        if domain == 'FACE' and mode == 'Not Connected':
            assert len(actual[0]) == len(mesh.vertices), 'Do not merge within one mesh island'
        checks += 1

# A nearby unselected point must remain separate even with an intentionally huge threshold.
selection.domain = 'POINT'
selection.inputs['Indices'].default_value = '0'
merge.inputs['Mode'].default_value = 'All'
merge.inputs['Distance'].default_value = 100
group.links.new(selection.outputs['Points'], merge.inputs['Selection'])
assert len(evaluate()[0][0]) == len(mesh.vertices)
checks += 1
print('GEOMETRY_VIEWER_MERGE_RECIPES_OK', checks)
