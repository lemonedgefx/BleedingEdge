# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Viewer extrusion adapter and mixed editing chains. Requires a Phase 2 build.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
The separate recipes test is the only test runnable on older builds.
"""

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = 'Viewer Extrude Test'
TREE = 'Viewer Extrude Test'


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


def refresh():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}


def select(domain):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    element = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain][0]
    element.select_set(True)
    bm.select_history.add(element)
    if domain == 'FACE':
        bm.faces.active = element
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def evaluated_branch(domain=None):
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
    operation = result_node()
    geometry = next(s for s in operation.outputs if s.type == 'GEOMETRY')
    store = None
    if domain:
        store = tree().nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type = 'BOOLEAN'
        store.domain = domain
        store.inputs['Name'].default_value = '.test_extrude_top'
        tree().links.new(geometry, store.inputs['Geometry'])
        tree().links.new(operation.outputs['Top'], store.inputs['Value'])
        geometry = store.outputs['Geometry']
    tree().links.new(geometry, output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        evaluated = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh(preserve_all_data_layers=True,
                                 depsgraph=bpy.context.evaluated_depsgraph_get())
        result = ([v.co.copy() for v in mesh.vertices],
                  [tuple(e.vertices) for e in mesh.edges],
                  [tuple(f.vertices) for f in mesh.polygons])
        top = [i for i, v in enumerate(mesh.attributes['.test_extrude_top'].data) if v.value] if domain else []
        assert not any(a.name.startswith('.viewer_extrude_top_') for a in mesh.attributes)
        evaluated.to_mesh_clear()
        return result, top
    finally:
        tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        if store:
            tree().nodes.remove(store)
        bpy.context.view_layer.update()


def assert_proxy(expected, top, domain):
    assert bpy.context.mode == 'EDIT_MESH'
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    assert (len(bm.verts), len(bm.edges), len(bm.faces)) == tuple(map(len, expected))
    for v, co in zip(bm.verts, expected[0]):
        assert (v.co - co).length < 1e-4
    elements = {'POINT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain]
    assert [e.index for e in elements if e.select] == top


bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
obj.name = SOURCE
obj.rotation_euler = (0.2, -0.4, 0.3)
obj.scale = (1.5, 0.7, 2)
parent = bpy.data.objects.new('Viewer Extrude Parent', None)
bpy.context.collection.objects.link(parent)
parent.rotation_euler = (-0.3, 0.2, 0.1)
parent.scale = (2, 0.5, 1.25)
obj.parent = parent
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = group.nodes.new('NodeGroupInput')
output_node = group.nodes.new('NodeGroupOutput')
viewer_node = group.nodes.new('GeometryNodeViewer')
viewer_node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], viewer_node.inputs['Geometry'])
obj.modifiers.new('Viewer Extrude Test', 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
original = [v.co.copy() for v in obj.data.vertices]
obj = group = input_node = output_node = viewer_node = parent = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Extrude Baseline')
begin()

# Empty E and an unsupported domain must not author an empty node or mutate the proxy.
before = len(tree().nodes)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_extrude.poll()
    assert bpy.ops.node.geometry_viewer_edit_extrude(offset=(1, 0, 0)) == {'CANCELLED'}
select('VERT')
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_extrude(individual=True) == {'CANCELLED'}
assert len(tree().nodes) == before

# The node's region kernel has no boundary to extrude when the entire closed cube is selected.
select('FACE')
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.select_all(action='SELECT')
    assert bpy.ops.node.geometry_viewer_edit_extrude() == {'CANCELLED'}
assert len(tree().nodes) == before and bpy.context.mode == 'OBJECT'
assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
begin()

for domain, individual in (('POINT', False), ('EDGE', False), ('FACE', False), ('FACE', True)):
    select({'POINT': 'VERT', 'EDGE': 'EDGE', 'FACE': 'FACE'}[domain])
    before = len(tree().nodes)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_extrude(offset=(0.2, -0.3, 0.4),
                                                        individual=individual) == {'FINISHED'}
    assert bpy.context.mode == 'OBJECT'  # Proxy retired before native global undo.
    assert len(tree().nodes) == before + 3
    assert result_node().bl_idname == 'GeometryNodeExtrudeMesh'
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)
    if domain == 'FACE':
        assert result_node().inputs['Individual'].default_value == individual
    expected, top = evaluated_branch(domain)
    refresh()
    assert_proxy(expected, top, domain)
    # The original object and final modifier output are still untouched.
    assert all((v.co - co).length < 1e-6 for v, co in zip(source().data.vertices, original))
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    assert next(l.from_node.bl_idname for l in tree().links if l.to_node == output) == 'NodeGroupInput'

# Face Top survives E -> G -> S -> E, with each capture referring to its own stage.
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.translate(value=(0.1, 0, 0), use_proportional_edit=False,
                                        mirror=False) == {'FINISHED'}
refresh()
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.2,) * 3, use_proportional_edit=False,
                                     mirror=False) == {'FINISHED'}
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Before scripted final extrusion')
refresh()
before = len(tree().nodes)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.node.geometry_viewer_edit_extrude(offset=(0, 0, 0)) == {'FINISHED'}
assert len(tree().nodes) == before + 3  # Zero movement still commits extrusion topology.
if bpy.app.background:
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.ed.undo_push(message='Scripted zero-offset extrusion')
refresh()
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.undo() == {'FINISHED'}
assert bpy.context.mode == 'OBJECT' and len(tree().nodes) == before
assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.ed.redo() == {'FINISHED'}
assert len(tree().nodes) == before + 3 and bpy.context.mode == 'OBJECT'
assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
print('GEOMETRY_VIEWER_EXTRUDE_SMOKE_OK')
