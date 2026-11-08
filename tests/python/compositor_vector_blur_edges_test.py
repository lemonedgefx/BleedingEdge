"""Spatial footprint coverage must survive directional opacity normalization."""

import os
import tempfile
import bpy
import numpy as np

device = os.environ.get('VB_DEVICE', 'CPU')
folder = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_edges_')
os.makedirs(folder, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = device
scene.render.resolution_x, scene.render.resolution_y = 129, 97
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'
tree = bpy.data.node_groups.new('spatial_coverage', 'CompositorNodeTree')
tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
scene.compositing_node_group = tree
node = tree.nodes.new('CompositorNodeVecBlur')
node.inputs['Samples'].default_value = 256
node.inputs['Shutter'].default_value = 2
node.inputs['Curved'].default_value = False
node.inputs['Falloff'].default_value = 'Smooth'
color = np.zeros((97, 129, 4), np.float32)
color[48, 64] = 1
speed = np.zeros_like(color)
speed[48, 64] = (-48, -24, -48, -24)
for label, pixels in [('Image', color), ('Speed', speed)]:
    im = bpy.data.images.new(label, 129, 97, alpha=True, float_buffer=True)
    im.alpha_mode = 'PREMUL'
    im.colorspace_settings.name = 'Non-Color'
    im.pixels.foreach_set(pixels.ravel())
    im.update()
    upstream = tree.nodes.new('CompositorNodeImage')
    upstream.image = im
    tree.links.new(upstream.outputs['Image'], node.inputs[label])
out = tree.nodes.new('NodeGroupOutput')
tree.links.new(node.outputs['Image'], out.inputs['Image'])
scene.render.filepath = os.path.join(folder, 'diagonal_point.exr')
bpy.ops.render.render(write_still=True)
im = bpy.data.images.load(scene.render.filepath)
im.colorspace_settings.name = 'Non-Color'
values = np.array(im.pixels[:], np.float32).reshape(color.shape)
centre, edge = float(values[56, 80, 3]), float(values[57, 80, 3])
print(f'GEOMETRIC_COVERAGE {device} centre={centre:.6f} edge={edge:.6f}', flush=True)
assert centre > 0.5, 'Opacity normalization lost the visible diagonal trail'
assert edge < 0.2 and edge < centre * 0.3, \
    f'Normalization makes a grazing footprint opaque, creating a hard contour: {edge}'
assert np.max(np.abs(values[:, :, 0] - values[:, :, 3])) < 1e-5, 'Premultiplied alpha differs'
assert values[58, 80, 3] < 1e-6, 'Blur leaves its pixel footprint'
print(f'VECTOR_BLUR_EDGES_OK {device}', flush=True)
