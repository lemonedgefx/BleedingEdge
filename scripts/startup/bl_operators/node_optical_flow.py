# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Editable, ready-wired compositor workflows using the native motion nodes."""

import bpy
from bpy.props import EnumProperty
from bpy.types import Operator

from .node import NodeAddOperator


def _socket(tree, name, socket_type, *, output=False, default=None, minimum=None, maximum=None):
    socket = tree.interface.new_socket(
        name=name, in_out='OUTPUT' if output else 'INPUT', socket_type=socket_type,
    )
    if default is not None:
        socket.default_value = default
    if minimum is not None:
        socket.min_value = minimum
    if maximum is not None:
        socket.max_value = maximum
    return socket


def build_optical_flow_group(mode):
    """Create a new group each time; never modify an existing user's node group."""
    if mode not in {'MOTION_BLUR', 'INTERPOLATE'}:
        raise ValueError("Unknown optical flow workflow")
    motion_blur = mode == 'MOTION_BLUR'
    tree = bpy.data.node_groups.new(
        'Footage Motion Blur' if motion_blur else 'Frame Interpolate', 'CompositorNodeTree',
    )
    try:
        if motion_blur:
            _socket(tree, 'Previous Image', 'NodeSocketColor', default=(0, 0, 0, 0))
        _socket(tree, 'Image', 'NodeSocketColor', default=(0, 0, 0, 0))
        _socket(tree, 'Next Image', 'NodeSocketColor', default=(0, 0, 0, 0))
        if motion_blur:
            _socket(tree, 'Shutter', 'NodeSocketFloat', default=0.5, minimum=0, maximum=2)
            _socket(tree, 'Samples', 'NodeSocketInt', default=32, minimum=1, maximum=256)
        else:
            _socket(tree, 'Factor', 'NodeSocketFloat', default=0.5, minimum=0, maximum=1)
        quality = _socket(tree, 'Quality', 'NodeSocketMenu')
        _socket(tree, 'Max Motion', 'NodeSocketFloat', default=64, minimum=0, maximum=2048)
        _socket(tree, 'Mask', 'NodeSocketFloat', default=1, minimum=0, maximum=1)
        _socket(tree, 'Confidence Threshold', 'NodeSocketFloat', default=0.01, minimum=0, maximum=1)
        fallback = None if motion_blur else _socket(tree, 'Fallback', 'NodeSocketMenu')
        _socket(tree, 'Image', 'NodeSocketColor', output=True)
        _socket(tree, 'Confidence', 'NodeSocketFloat', output=True)
        _socket(tree, 'Motion Preview', 'NodeSocketColor', output=True)
        inputs = tree.nodes.new('NodeGroupInput')
        inputs.location = (-360, 120)
        output = tree.nodes.new('NodeGroupOutput')
        output.location = (800, 120)

        def link(source, source_name, destination, destination_name):
            tree.links.new(source.outputs[source_name], destination.inputs[destination_name])

        first = tree.nodes.new('CompositorNodeOpticalFlow')
        first.label = 'Current → Previous' if motion_blur else 'First → Second'
        first.location = (0, 260)
        second = tree.nodes.new('CompositorNodeOpticalFlow')
        second.label = 'Current → Next' if motion_blur else 'Second → First'
        second.location = (0, -120)
        for flow in (first, second):
            for name in ('Quality', 'Max Motion', 'Mask'):
                link(inputs, name, flow, name)
            # Interpolation checks both fields itself; do not estimate them twice again.
            flow.inputs['Consistency Check'].default_value = motion_blur
        quality.default_value = 'Balanced'
        link(inputs, 'Image', first, 'Image')
        link(inputs, 'Previous Image' if motion_blur else 'Next Image', first, 'Next Image')
        link(inputs, 'Image' if motion_blur else 'Next Image', second, 'Image')
        link(inputs, 'Next Image' if motion_blur else 'Image', second, 'Next Image')

        if motion_blur:
            adapter = tree.nodes.new('CompositorNodeFlowToSpeed')
            adapter.location = (280, 220)
            link(first, 'Flow', adapter, 'Previous Flow')
            link(first, 'Confidence', adapter, 'Previous Confidence')
            link(second, 'Flow', adapter, 'Next Flow')
            link(second, 'Confidence', adapter, 'Next Confidence')
            link(inputs, 'Confidence Threshold', adapter, 'Confidence Threshold')
            blur = tree.nodes.new('CompositorNodeVecBlur')
            blur.location = (540, 260)
            link(inputs, 'Image', blur, 'Image')
            link(inputs, 'Shutter', blur, 'Shutter')
            link(inputs, 'Samples', blur, 'Samples')
            link(adapter, 'Speed', blur, 'Speed')
            # Leave Depth at its uniform default: this is 2D footage, not inferred depth.
            link(blur, 'Image', output, 'Image')
            link(adapter, 'Confidence', output, 'Confidence')
            preview_flow = second
        else:
            interpolate = tree.nodes.new('CompositorNodeFlowInterpolate')
            interpolate.location = (280, 260)
            for name in ('Image', 'Next Image', 'Factor', 'Fallback', 'Confidence Threshold'):
                link(inputs, name, interpolate, name)
            fallback.default_value = 'Crossfade'
            link(first, 'Flow', interpolate, 'Forward Flow')
            link(first, 'Confidence', interpolate, 'Forward Confidence')
            link(second, 'Flow', interpolate, 'Backward Flow')
            link(second, 'Confidence', interpolate, 'Backward Confidence')
            link(interpolate, 'Image', output, 'Image')
            link(interpolate, 'Confidence', output, 'Confidence')
            preview_flow = first
        preview = tree.nodes.new('CompositorNodeFlowVisualize')
        preview.location = (540, -140)
        link(preview_flow, 'Flow', preview, 'Flow')
        link(preview_flow, 'Confidence', preview, 'Confidence')
        link(inputs, 'Max Motion', preview, 'Display Range')
        link(preview, 'Image', output, 'Motion Preview')
        return tree
    except Exception:
        bpy.data.node_groups.remove(tree)
        raise


class NODE_OT_add_optical_flow_workflow(NodeAddOperator, Operator):
    bl_idname = 'node.add_optical_flow_workflow'
    bl_label = 'Add Optical Flow Workflow'
    bl_description = 'Add an editable motion workflow, optionally connected to the selected Image node'
    bl_options = {'REGISTER', 'UNDO'}

    mode: EnumProperty(
        name='Workflow',
        items=(
            ('MOTION_BLUR', 'Footage Motion Blur', 'Estimate past/future motion and blur footage'),
            ('INTERPOLATE', 'Frame Interpolate', 'Generate an in-between image from two frames'),
        ),
    )

    @classmethod
    def poll(cls, context):
        return (super().poll(context) and
                context.space_data.edit_tree.bl_idname == 'CompositorNodeTree')

    def execute(self, context):
        tree = context.space_data.edit_tree
        source = tree.nodes.active
        group = build_optical_flow_group(self.mode)
        node = None
        try:
            node = tree.nodes.new('CompositorNodeGroup')
            node.node_tree = group
            node.width = 240
            if (source and source.bl_idname == 'CompositorNodeImage' and source.image and
                    source.image.type != 'MULTILAYER' and
                    all(name in source.outputs and not source.outputs[name].is_unavailable
                        for name in ('Image', 'Previous Frame', 'Next Frame'))):
                node.parent = source.parent
                node.location = (source.location.x + source.width + 80, source.location.y)
                height = max(240, 25 * (len(node.inputs) + len(node.outputs) + 6))

                def overlaps(other):
                    return (other not in (node, source) and other.parent == node.parent and
                            node.location.x < other.location.x + other.width + 40 and
                            node.location.x + node.width + 40 > other.location.x and
                            node.location.y > other.location.y - max(240, other.dimensions.y) - 40 and
                            node.location.y - height - 40 < other.location.y)

                while any(overlaps(other) for other in tree.nodes):
                    node.location.y -= height + 80
                tree.links.new(source.outputs['Image'], node.inputs['Image'])
                tree.links.new(source.outputs['Next Frame'], node.inputs['Next Image'])
                if self.mode == 'MOTION_BLUR':
                    tree.links.new(source.outputs['Previous Frame'], node.inputs['Previous Image'])
            else:
                node.location = context.space_data.cursor_location
            for other in tree.nodes:
                other.select = other == node
            tree.nodes.active = node
        except Exception:
            if node is not None:
                tree.nodes.remove(node)
            bpy.data.node_groups.remove(group)
            raise
        return {'FINISHED'}


classes = (NODE_OT_add_optical_flow_workflow,)
