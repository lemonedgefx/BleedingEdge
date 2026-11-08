"""Custom Vector Blur shutter windows, preset UI data, and saved-node compatibility."""

import os
from pathlib import Path
import tempfile

import bpy
import numpy as np

device = os.environ.get('VB_DEVICE', 'CPU')
folder = Path(os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_shutter_'))
folder.mkdir(parents=True, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = device
scene.render.resolution_x = 257
scene.render.resolution_y = 129
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'
tree = bpy.data.node_groups.new('Shutter window', 'CompositorNodeTree')
tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
scene.compositing_node_group = tree
node = tree.nodes.new('CompositorNodeVecBlur')
assert node.inputs['Falloff'].default_value == 'Linear', 'New nodes must default to Linear'
assert node.shutter_curve is not None, 'Native editable shutter curve missing'
node.inputs['Samples'].default_value = 256
node.inputs['Shutter'].default_value = 2
node.inputs['Curved'].default_value = False


def set_image(socket, values):
    height, width = values.shape[:2]
    image = bpy.data.images.new(socket, width, height, alpha=True, float_buffer=True)
    image.alpha_mode = 'PREMUL'
    image.colorspace_settings.name = 'Non-Color'
    image.pixels.foreach_set(values.ravel())
    image.update()
    image.pack()
    upstream = tree.nodes.new('CompositorNodeImage')
    upstream.image = image
    tree.links.new(upstream.outputs['Image'], node.inputs[socket])
    return image


def render(name):
    scene.render.filepath = str(folder / (name + '.exr'))
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath)
    image.colorspace_settings.name = 'Non-Color'
    values = np.array(image.pixels[:], np.float32).reshape((129, 257, 4))
    bpy.data.images.remove(image)
    assert np.isfinite(values).all(), name + ': non-finite result'
    np.save(folder / (name + '.npy'), values)
    return values


def ramp(start, end):
    mapping = node.shutter_curve
    points = mapping.curves[0].points
    while len(points) > 2:
        points.remove(points[1])
    points[0].location = (0, start)
    points[1].location = (1, end)
    for point in points:
        point.handle_type = 'VECTOR'
    mapping.update()
    tree.update_tag()
    node.inputs['Falloff'].default_value = 'Custom'


color = np.zeros((129, 257, 4), np.float32)
color[62:67, 126:131] = 1
speed = np.zeros_like(color)
speed[62:67, 126:131] = (-48, 0, -48, 0)
source = set_image('Image', color)
vectors = set_image('Speed', speed)
out = tree.nodes.new('NodeGroupOutput')
tree.links.new(node.outputs['Image'], out.inputs['Image'])

linear = render('linear_default')
node.inputs['Falloff'].default_value = 'Custom'
custom = render('custom_linear')
assert np.max(np.abs(custom - linear)) < 0.003, 'Linear curve differs from Linear preset'

# All six buttons use the same native preset shapes as Render > Motion Blur.
for preset in ['SMOOTH', 'ROUND', 'ROOT', 'SHARP', 'LINE', 'MAX']:
    node.shutter_curve_preset = preset
    assert node.inputs['Falloff'].default_value == 'Custom'
    mapping = node.shutter_curve
    assert mapping.evaluate(mapping.curves[0], 0.5) > 0.99, preset + ': centre is not open'
    ends = [mapping.evaluate(mapping.curves[0], time) for time in [0, 1]]
    assert all(abs(value - (1 if preset == 'MAX' else 0)) < 0.001 for value in ends)

# A full shutter curve controls both halves independently; it is not mirrored.
ramp(0, 1)
asymmetric = render('asymmetric')
left, right = (float(asymmetric[64, x, 3]) for x in [104, 152])
print(f'SHUTTER_ASYMMETRY {device} {left:.6f} {right:.6f}', flush=True)
assert left < 0.4 and right > 0.6 and right - left > 0.3
assert np.max(np.abs(asymmetric[:, :, 0] - asymmetric[:, :, 3])) < 1e-5

# A second, slower source must traverse the full curve over its own shorter path.
color[94:99, 126:131] = 1
speed[94:99, 126:131] = (-24, 0, -24, 0)
source.pixels.foreach_set(color.ravel())
source.update()
vectors.pixels.foreach_set(speed.ravel())
vectors.update()
varied = render('per_source_time')
assert abs(float(varied[96, 140, 3]) - float(varied[64, 152, 3])) < 0.06
assert varied[97, 155, 3] < 1e-4, 'A rounded endpoint gained a visible out-of-support pixel'

# Nonlinear reconstruction uses the same shutter curve and normalizes the full
# interval once. Normalizing each half separately would erase its asymmetry.
color[94:99, 126:131] = 0
speed[:] = 0
speed[62:67, 126:131] = (-48, -24, -48, 24)
source.pixels.foreach_set(color.ravel())
source.update()
vectors.pixels.foreach_set(speed.ravel())
vectors.update()
node.inputs['Curved'].default_value = True
arc = render('curved_asymmetric')
left_energy = float(arc[:, :125, 3].sum())
right_energy = float(arc[:, 132:, 3].sum())
assert right_energy > left_energy * 1.7, 'Curved mode discarded shutter asymmetry'
assert abs(float(arc[:, :, 3].sum()) - 25) < 0.3, 'Curved shutter changed source energy'

# Curve copies must own their control points independently.
copy = tree.copy()
copy_node = next(n for n in copy.nodes if n.bl_idname == 'CompositorNodeVecBlur')
copy_node.shutter_curve.curves[0].points[0].location.y = 0.8
copy_node.shutter_curve.update()
assert abs(node.shutter_curve.curves[0].points[0].location.y) < 1e-6
bpy.data.node_groups.remove(copy)

source.pack()
vectors.pack()
saved_path = str(folder / 'shutter_curve.blend')
bpy.ops.wm.save_as_mainfile(filepath=saved_path)
bpy.ops.wm.open_mainfile(filepath=saved_path)
scene = bpy.context.scene
tree = scene.compositing_node_group
node = next(n for n in tree.nodes if n.bl_idname == 'CompositorNodeVecBlur')
assert node.inputs['Falloff'].default_value == 'Custom'
assert abs(node.shutter_curve.curves[0].points[0].location.y) < 1e-6
assert abs(node.shutter_curve.curves[0].points[-1].location.y - 1) < 1e-6
assert np.max(np.abs(render('reloaded') - arc)) < 0.003, 'Curve lost on save/reload'

ramp(0, 0)
for curved in [False, True]:
    node.inputs['Curved'].default_value = curved
    assert np.max(np.abs(render('closed_' + str(curved)))) < 1e-6, 'Closed shutter has exposure'

legacy = os.environ.get('VB_LEGACY_BLEND')
if legacy:
    bpy.ops.wm.open_mainfile(filepath=legacy)
    old = bpy.context.scene.compositing_node_group
    for old_node in old.nodes:
        if old_node.bl_idname == 'CompositorNodeVecBlur':
            assert old_node.shutter_curve is not None, 'Older node did not acquire curve storage'
            assert old_node.inputs['Falloff'].default_value == 'Smooth', 'Saved falloff was changed'

print(f'VECTOR_BLUR_SHUTTER_OK {device}', flush=True)
