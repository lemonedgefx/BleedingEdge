"""Vector Blur taper, opaque background, temporal symmetry and persistence regressions.

Run with --python-exit-code 1. VB_DEVICE selects CPU or GPU.
VB_FIXTURE_ROOT optionally adds the captured rotating-bar regression from the user's scene.
"""

import os
import tempfile

import bpy
import numpy as np


DEVICE = os.environ.get("VB_DEVICE", "CPU")
OUTPUT = os.environ.get("VB_OUTPUT") or tempfile.mkdtemp(prefix="vector_blur_falloff_")
os.makedirs(OUTPUT, exist_ok=True)
scene = bpy.context.scene
scene.render.compositor_device = DEVICE
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.view_settings.view_transform = 'Raw'
scene.render.resolution_percentage = 100


def image(name, values):
    h, w = values.shape[:2]
    result = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    result.alpha_mode = 'PREMUL'
    result.pixels.foreach_set(values.astype(np.float32).ravel())
    result.update()
    return result


def render(source, speed, falloff, tag, curved=False, samples=128, shutter=2.0):
    scene.render.resolution_x, scene.render.resolution_y = source.size
    tree = bpy.data.node_groups.new(tag, "CompositorNodeTree")
    tree.interface.new_socket("Image", in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    node = tree.nodes.new("CompositorNodeVecBlur")
    node.inputs['Samples'].default_value = samples
    node.inputs['Shutter'].default_value = shutter
    node.inputs['Curved'].default_value = curved
    if 'Falloff' in node.inputs:
        node.inputs['Falloff'].default_value = falloff
    else:
        assert falloff == 'None', "Falloff dropdown missing"
    for name, value in [('Image', source), ('Speed', speed)]:
        upstream = tree.nodes.new("CompositorNodeImage")
        upstream.image = value
        tree.links.new(upstream.outputs['Image'], node.inputs[name])
    out = tree.nodes.new("NodeGroupOutput")
    tree.links.new(node.outputs['Image'], out.inputs['Image'])
    path = os.path.join(OUTPUT, tag + '.exr')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    result = bpy.data.images.load(path)
    values = np.empty(source.size[0] * source.size[1] * 4, dtype=np.float32)
    result.pixels.foreach_get(values)
    return values.reshape(source.size[1], source.size[0], 4)


# A finite rotating bar surrounded by stationary background, unlike a uniform speed field.
size = 256
y, x = np.indices((size, size))
dx, dy = x - size / 2, y - size / 2
angle = 0.6
u = dx * np.cos(angle) + dy * np.sin(angle)
v = -dx * np.sin(angle) + dy * np.cos(angle)
mask = (np.abs(u) < 75) & (np.abs(v) < 3)
source_values = np.zeros((size, size, 4), dtype=np.float32)
source_values[:, :, 3] = 1
source_values[mask, :3] = 1
speed_values = np.zeros_like(source_values)
for channel, rotation, sign in [(0, -0.39, 1), (2, 0.39, -1)]:
    rx = dx * np.cos(rotation) - dy * np.sin(rotation) - dx
    ry = dx * np.sin(rotation) + dy * np.cos(rotation) - dy
    speed_values[mask, channel] = sign * rx[mask]
    speed_values[mask, channel + 1] = sign * ry[mask]
bar = image('rotating_bar', source_values)
bar_speed = image('rotating_bar_speed', speed_values)
normal_distance = v
blur = render(bar, bar_speed, 'None', 'rotating_bar', curved=True)
previous = float(blur[normal_distance < -7, 0].sum())
later = float(blur[normal_distance > 7, 0].sum())
balance = min(previous, later) / max(previous, later)
print(f"ROTATING_BAR_BALANCE {DEVICE} {balance:.6f}", flush=True)
assert balance > 0.85, f"Rotating foreground lost one shutter side: {balance}"

blur = render(bar, bar_speed, 'Smooth', 'rotating_bar_smooth', curved=True)
previous = float(blur[normal_distance < -7, 0].sum())
later = float(blur[normal_distance > 7, 0].sum())
balance = min(previous, later) / max(previous, later)
print(f"ROTATING_TAPER_BALANCE {DEVICE} {balance:.6f}", flush=True)
assert balance > 0.85, f"Taper lost one shutter side on a rotating foreground: {balance}"


fixture = os.environ.get('VB_FIXTURE_ROOT')
if fixture:
    source_values = np.load(os.path.join(fixture, 'new_source.npy'))
    speed_values = np.load(os.path.join(fixture, 'new_speed.npy'))
    source = image('captured_source', source_values)
    speed = image('captured_speed', speed_values)
    result = render(source, speed, 'None', 'captured_rotating_bar',
                    curved=True, samples=32, shutter=1.68)
    grey = source_values[:, :, :3].mean(2)
    bg = float(np.median(grey))
    mask = grey > bg + 0.5
    ys, xs = np.nonzero(mask)
    center = np.array([xs.mean(), ys.mean()])
    _, axes = np.linalg.eigh(np.cov(np.stack([xs, ys], axis=1).T))
    axis = axes[:, -1]
    normal = np.array([-axis[1], axis[0]])
    yy, xx = np.indices(mask.shape)
    signed = (xx - center[0]) * normal[0] + (yy - center[1]) * normal[1]
    energy = np.maximum(result[:, :, :3].mean(2) - bg, 0)
    previous = float(energy[signed < -7].sum())
    later = float(energy[signed > 7].sum())
    balance = min(previous, later) / max(previous, later)
    print(f"CAPTURED_BAR_BALANCE {DEVICE} {balance:.6f}", flush=True)
    assert balance > 0.9, f"User scene still has an unequal trail: {balance}"


size = 128
source_values = np.zeros((size, size, 4), dtype=np.float32)
source_values[:, :, 3] = 1
source_values[62:67, 62:67, :3] = 1
speed_values = np.zeros_like(source_values)
speed_values[62:67, 62:67, 0] = -48
speed_values[62:67, 62:67, 2] = -48
source = image('opaque_marker', source_values)
speed = image('finite_marker_speed', speed_values)
outputs = {}
for falloff in ['Linear', 'Smooth', 'Ease In', 'Ease Out']:
    result = render(source, speed, falloff, falloff.replace(' ', '_'))
    outputs[falloff] = result
    assert float(result[64, 64, 0]) > 0.8, \
        f"{falloff}: the opacity envelope was diluted by exposure averaging"
    row = result[64, :, 0]
    near, middle, far = (float(row[64 + distance]) for distance in [12, 24, 40])
    assert near > middle > far > 0, f"{falloff}: trail does not fade: {near, middle, far}"
    assert row[116] < 1e-5, f"{falloff}: trail extends beyond the endpoint"
    assert np.max(np.abs(result[:, :, 3] - 1)) < 1e-5, f"{falloff}: opaque background lost alpha"
    print(f"FALLOFF_OK {DEVICE} {falloff}: {near:.4f}, {middle:.4f}, {far:.4f}", flush=True)

assert outputs['Ease Out'][64, 104, 0] < outputs['Linear'][64, 104, 0] < \
       outputs['Ease In'][64, 104, 0], "Dropdown choices do not produce distinct taper shapes"

# Samples now changes directional image filtering directly. Sparse or differently
# phased samples can change a small marker's local values, so test normalization
# independently with a uniform opaque image instead of demanding identical detail.
uniform_values = np.ones_like(source_values)
uniform = image('uniform_sample_normalization', uniform_values)
for count in [1, 64, 128]:
    result = render(uniform, speed, 'Smooth', f'sample_count_{count}', samples=count)
    assert np.max(np.abs(result - uniform_values)) < 1e-5, \
        f"Samples {count} changes uniform colour or opacity"
for distance in [12, 24, 40]:
    left, right = outputs['Smooth'][64, 64 - distance, 0], outputs['Smooth'][64, 64 + distance, 0]
    assert abs(float(left - right)) < 0.05, "Taper is unequal on the two shutter sides"

result = render(source, speed, 'Smooth', 'zero_shutter', shutter=0)
assert np.max(np.abs(result - source_values)) < 1e-5, "Zero shutter changed the input"

# Transparent premultiplied input must keep RGB and alpha in agreement.
source_values[:, :, 3] = source_values[:, :, 0]
transparent = image('transparent_marker', source_values)
result = render(transparent, speed, 'Smooth', 'transparent')
assert np.max(np.abs(result[:, :, 0] - result[:, :, 3])) < 1e-5, "Premultiplied alpha was broken"
assert float(result[64, 88, 3]) > float(result[64, 104, 3]) > 0

full_transparent_result = result.copy()
half = image('half_alpha_marker', source_values * 0.5)
result = render(half, speed, 'Smooth', 'half_alpha')
assert np.max(np.abs(result - full_transparent_result * 0.5)) < 1e-5, \
    "Translucent foreground exposure did not scale with alpha"
assert np.max(np.abs(result[:, :, 0] - result[:, :, 3])) < 1e-5

# The opacity mode must use the same curved path, rather than fading a straight chord.
speed_values[62:67, 62:67, 1] = -24
speed_values[62:67, 62:67, 3] = 24
arc_speed = image('arc_speed', speed_values)
result = render(transparent, arc_speed, 'Smooth', 'curved_taper', curved=True)
for distance in [12, 24, 36]:
    column = result[:, 64 + distance, 0]
    predicted = 64 - 24 * (distance / 48) ** 2
    assert float(column.max()) > 0.005 and abs(int(column.argmax()) - predicted) < 4, \
        "Falloff lost the curved trajectory"

# Zero shutter, zero motion, and stationary colored regions remain exact.
zero = image('zero_speed', np.zeros_like(speed_values))
result = render(source, zero, 'Smooth', 'stationary')
assert np.max(np.abs(result - np.asarray(list(source.pixels)).reshape(size, size, 4))) < 1e-5

node = next(n for n in scene.compositing_node_group.nodes if n.bl_idname == 'CompositorNodeVecBlur')
node.inputs['Falloff'].default_value = 'Ease Out'
node.inputs['Shutter'].default_value = 0
source.pack()
zero.pack()
path = os.path.join(OUTPUT, 'falloff_persistence.blend')
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
node = next(n for n in bpy.context.scene.compositing_node_group.nodes
            if n.bl_idname == 'CompositorNodeVecBlur')
assert node.inputs['Falloff'].default_value == 'Ease Out', "Falloff was not saved"
print(f"VECTOR_BLUR_FALLOFF_OK {DEVICE}", flush=True)
