"""Directional falloff is the streak opacity, independent of exposure averaging.

VB_DEVICE selects CPU or GPU. Run with --python-exit-code 1.
"""

import os
import tempfile

import bpy
import numpy as np


DEVICE = os.environ.get('VB_DEVICE', 'CPU')
OUTPUT = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_opacity_')
os.makedirs(OUTPUT, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = DEVICE
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'


def image(name, pixels):
    height, width = pixels.shape[:2]
    result = bpy.data.images.new(name, width, height, alpha=True, float_buffer=True)
    result.alpha_mode = 'PREMUL'
    result.colorspace_settings.name = 'Non-Color'
    result.pixels.foreach_set(pixels.astype(np.float32).ravel())
    result.update()
    return result


def render(pixels, vectors, name, falloff='Smooth', samples=256):
    height, width = pixels.shape[:2]
    scene.render.resolution_x, scene.render.resolution_y = width, height
    scene.render.resolution_percentage = 100
    tree = bpy.data.node_groups.new(name, 'CompositorNodeTree')
    tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    node = tree.nodes.new('CompositorNodeVecBlur')
    node.inputs['Samples'].default_value = samples
    node.inputs['Shutter'].default_value = 2
    node.inputs['Curved'].default_value = False
    node.inputs['Falloff'].default_value = falloff
    for socket, values in [('Image', pixels), ('Speed', vectors)]:
        upstream = tree.nodes.new('CompositorNodeImage')
        upstream.image = image(name + socket, values)
        tree.links.new(upstream.outputs['Image'], node.inputs[socket])
    output = tree.nodes.new('NodeGroupOutput')
    tree.links.new(node.outputs['Image'], output.inputs['Image'])
    path = os.path.join(OUTPUT, name + '.exr')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    loaded = bpy.data.images.load(path)
    loaded.colorspace_settings.name = 'Non-Color'
    result = np.array(loaded.pixels[:], np.float32).reshape(height, width, 4)
    bpy.data.images.remove(loaded)
    assert np.isfinite(result).all(), f'{name}: non-finite result'
    return result


pixels = np.zeros((128, 256, 4), np.float32)
vectors = np.zeros_like(pixels)
pixels[61:66, 126:131] = 1
vectors[61:66, 126:131] = (-48, 0, -48, 0)
result = render(pixels, vectors, 'transparent_envelope')
peak = float(result[63, 128, 3])
print(f'OPACITY_PEAK {DEVICE} {peak:.6f}', flush=True)
assert peak > 0.9, f'Falloff still multiplies an already diluted streak: peak={peak}'
expected = [0.84375, 0.5, 0.15625]
for direction in [-1, 1]:
    actual = [float(result[63, 128 + direction * d, 3]) for d in [12, 24, 36]]
    assert np.max(np.abs(np.array(actual) - expected)) < 0.08, \
        f'Opacity is not the requested 1-to-0 curve: {actual}'
assert result[63, 180, 3] < 1e-5, 'Opacity does not end at zero'
assert np.max(np.abs(result[:, :, :3] - result[:, :, 3:4])) < 1e-5, \
    'Premultiplied white colour differs from its opacity'

# An opaque background must show the same foreground envelope in its contrast.
opaque = pixels.copy()
opaque[:, :, :3] += (1 - opaque[:, :, 3:4]) * 0.04
opaque[:, :, 3] = 1
result = render(opaque, vectors, 'opaque_envelope')
actual = [(float(result[63, 128 + d, 0]) - 0.04) / 0.96 for d in [12, 24, 36]]
assert np.max(np.abs(np.array(actual) - expected)) < 0.08, \
    f'Opaque background changes the foreground envelope: {actual}'
assert np.max(np.abs(result[:, :, 3] - 1)) < 1e-5, 'Opaque background loses alpha'

# Source opacity and HDR colour are retained, rather than all sources becoming opaque.
half = pixels * 0.5
half[:, :, 0] *= 4
result = render(half, vectors, 'translucent_hdr')
assert abs(float(result[63, 152, 3]) - 0.25) < 0.04, 'Intrinsic opacity was discarded'
assert np.max(np.abs(result[:, :, 0] - 4 * result[:, :, 3])) < 1e-5, 'HDR colour was clipped'

# Two lengths in one tile must each fade over their own extent.
pixels[:] = 0
vectors[:] = 0
pixels[70:75, 126:131] = 1
pixels[82:87, 126:131] = 1
vectors[70:75, 126:131] = (-24, 0, -24, 0)
vectors[82:87, 126:131] = (-96, 0, -96, 0)
result = render(pixels, vectors, 'source_relative_envelopes')
for row, length in [(72, 24), (84, 96)]:
    for phase, target in [(0.5, 0.5), (0.75, 0.15625)]:
        value = float(result[row, 128 + int(length * phase), 3])
        assert abs(value - target) < 0.08, f'Another pixel sets the falloff: {row}, {value}'

# Colour is still directionally filtered; no separate sharp source is overlaid.
pixels[:] = 0
vectors[:] = 0
pixels[61:66, 116:141, 3] = 1
pixels[61:66, 116:141:2, :3] = 1
vectors[61:66, 116:141] = (-48, 0, -48, 0)
result = render(pixels, vectors, 'filtered_colour')
colour = float(result[63, 128, 0] / result[63, 128, 3])
assert 0.3 < colour < 0.7, f'Sharp source pixels were retained: {colour}'
print(f'VECTOR_BLUR_OPACITY_OK {DEVICE}', flush=True)
