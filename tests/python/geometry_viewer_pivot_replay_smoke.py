# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Selection-based pivots follow deformation; cursor/explicit pivots stay fixed."""

import os
import tempfile

import bmesh
import bpy
from mathutils import Matrix, Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def run_case(domain, kind, pivot_mode, persist=False):
    print('PIVOT_CASE', domain, kind, pivot_mode, flush=True)
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    group = bpy.data.node_groups.new('Pivot Replay', 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    source = group.nodes.new('NodeGroupInput')
    output = group.nodes.new('NodeGroupOutput')
    upstream = group.nodes.new('GeometryNodeSetPosition')
    viewer = group.nodes.new('GeometryNodeViewer')
    viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
    group.links.new(source.outputs['Geometry'], upstream.inputs['Geometry'])
    group.links.new(source.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(upstream.outputs['Geometry'], viewer.inputs['Geometry'])
    obj.modifiers.new('Pivot Replay', 'NODES').node_group = group
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    group.nodes.active = viewer
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
    element = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain][0]
    element.select_set(True)
    if domain == 'VERT':
        bm.verts[1].select_set(True)  # A single vertex about itself is a native no-op.
    bm.select_history.add(element)
    if domain == 'FACE':
        bm.faces.active = element
    bm.select_flush_mode()
    selected = {v.index for v in bm.verts if v.select}
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
    bm = element = None
    bpy.context.scene.tool_settings.transform_pivot_point = (
        'MEDIAN_POINT' if pivot_mode == 'OVERRIDE' else pivot_mode)
    bpy.context.scene.cursor.location = (0.25, -0.75, 1.25)
    options = dict(use_proportional_edit=False, mirror=False)
    if pivot_mode == 'OVERRIDE':
        options['center_override'] = (0.3, -0.6, 0.8)
    with bpy.context.temp_override(**override(VIEW_AREA)):
        if kind == 'SCALE':
            assert bpy.ops.transform.resize(value=(1.4,) * 3, **options) == {'FINISHED'}
            linear = Matrix.Diagonal((1.4, 1.4, 1.4))
        else:
            assert bpy.ops.transform.rotate(value=0.4, orient_type='GLOBAL', orient_axis='X',
                                            constraint_axis=(True, False, False),
                                            **options) == {'FINISHED'}
            linear = Matrix.Rotation(0.4, 3, 'X')
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE')
        if bpy.context.mode == 'EDIT_MESH':
            assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == obj
    operation = next(l.from_node for l in group.links if l.to_node == viewer)
    capture = (next(n for n in operation.node_tree.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
               if kind == 'ROTATE' else next(l.from_node for l in group.links if l.to_node == operation and
                                             l.to_socket.name == 'Selection'))
    assert capture.bl_idname == 'GeometryNodeSelectByIndex' and capture.mode == 'CAPTURED'

    def evaluate(eval_group=None):
        eval_group = eval_group or group
        eval_operation = eval_group.nodes[operation.name]
        eval_capture = (eval_operation.node_tree.nodes[capture.name] if kind == 'ROTATE' else
                        eval_group.nodes[capture.name])
        eval_output = eval_group.nodes[output.name]
        eval_source = eval_group.nodes[source.name]
        store = eval_group.nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type = 'BOOLEAN'
        store.domain = 'POINT'
        store.inputs['Name'].default_value = 'pivot_valid'
        eval_group.links.new(next(s for s in eval_operation.outputs if s.type == 'GEOMETRY'), store.inputs['Geometry'])
        validity_socket = None
        if kind == 'ROTATE':
            # Expose Valid only inside this diagnostic evaluation, then restore the public group.
            nested = eval_operation.node_tree
            validity_socket = nested.interface.new_socket(name='Capture Valid', in_out='OUTPUT',
                                                          socket_type='NodeSocketBool')
            nested_output = next(n for n in nested.nodes if n.bl_idname == 'NodeGroupOutput')
            nested.links.new(eval_capture.outputs['Valid'], nested_output.inputs[validity_socket.identifier])
            eval_group.links.new(eval_operation.outputs[validity_socket.identifier], store.inputs['Value'])
        else:
            eval_group.links.new(eval_capture.outputs['Valid'], store.inputs['Value'])
        eval_group.links.new(store.outputs['Geometry'], eval_output.inputs['Geometry'])
        try:
            bpy.context.view_layer.update()
            depsgraph = bpy.context.evaluated_depsgraph_get()
            evaluated = obj.evaluated_get(depsgraph)
            mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
            assert all(v.value for v in mesh.attributes['pivot_valid'].data)
            positions = [v.co.copy() for v in mesh.vertices]
            evaluated.to_mesh_clear()
            return positions
        finally:
            eval_group.links.new(eval_source.outputs['Geometry'], eval_output.inputs['Geometry'])
            eval_group.nodes.remove(store)
            if validity_socket is not None:
                eval_operation.node_tree.interface.remove(validity_socket)

    before = evaluate()
    delta = Vector((0.45, -0.3, 0.7))
    upstream.inputs['Offset'].default_value = delta
    after = evaluate()
    for i, (a, b) in enumerate(zip(after, before)):
        adjustment = (linear @ delta if i in selected and pivot_mode in {'CURSOR', 'OVERRIDE'}
                      else delta)
        assert (a - b - adjustment).length < 3e-5, (domain, kind, pivot_mode, i, a, b, adjustment)
    copied = group.copy()
    obj.modifiers[0].node_group = copied
    copied_positions = evaluate(copied)
    assert all((a - b).length < 3e-5 for a, b in zip(copied_positions, after))
    obj.modifiers[0].node_group = group
    bpy.data.node_groups.remove(copied)
    if persist:
        object_name, group_name = obj.name, group.name
        node_names = [node.name for node in (source, output, upstream, viewer, operation)]
        capture_name = capture.name
        with tempfile.TemporaryDirectory(prefix='viewer-pivot-test-') as directory:
            path = os.path.join(directory, 'pivot.blend')
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            obj = bpy.data.objects[object_name]
            group = bpy.data.node_groups[group_name]
            source, output, upstream, viewer, operation = [group.nodes[name] for name in node_names]
            capture = (operation.node_tree.nodes[capture_name] if kind == 'ROTATE' else
                       group.nodes[capture_name])
            restored = evaluate()
            assert all((a - b).length < 3e-5 for a, b in zip(restored, after))
        print('VIEWER_PIVOT_COPY_SAVE_RELOAD_OK', flush=True)
    mesh_data = obj.data
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.meshes.remove(mesh_data)
    bpy.data.node_groups.remove(group)
    if not persist:
        NODE_AREA.type = 'DOPESHEET_EDITOR'


cases = 0
for domain in ('VERT', 'EDGE', 'FACE'):
    for kind in ('SCALE', 'ROTATE'):
        for pivot_mode in ('CURSOR', 'OVERRIDE', 'MEDIAN_POINT', 'ACTIVE_ELEMENT'):
            run_case(domain, kind, pivot_mode,
                     persist=domain == 'FACE' and kind == 'ROTATE' and pivot_mode == 'ACTIVE_ELEMENT')
            cases += 1
print('GEOMETRY_VIEWER_PIVOT_REPLAY_SMOKE_OK', cases, flush=True)
