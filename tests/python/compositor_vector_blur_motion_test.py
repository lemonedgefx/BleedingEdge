"""Curved Vector Blur connection, constant-input, reversal, and CPU/GPU regressions.

Run with --python-exit-code 1. VB_DEVICE selects CPU or GPU.
"""

import bpy
import os
import random
import tempfile


SIZE = 128
OUTPUT = os.environ.get("VB_OUTPUT") or tempfile.mkdtemp(prefix="vector_blur_motion_")
os.makedirs(OUTPUT, exist_ok=True)
DEVICE = os.environ.get("VB_DEVICE", "CPU")


def image(name, values):
    result = bpy.data.images.new(name, SIZE, SIZE, alpha=True, float_buffer=True)
    result.pixels.foreach_set(values)
    result.update()
    return result


def constant_image(name, value):
    return image(name, list(value) * (SIZE * SIZE))


pixels = [0.0] * (SIZE * SIZE * 4)
for y in range(SIZE // 2 - 1, SIZE // 2 + 2):
    for x in range(SIZE // 2 - 1, SIZE // 2 + 2):
        start = (y * SIZE + x) * 4
        pixels[start:start + 4] = [1.0] * 4
source = image("marker", pixels)


def render(speed, extended, curved=True, tag="test"):
    scene = bpy.context.scene
    scene.render.compositor_device = DEVICE
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.view_settings.view_transform = 'Raw'
    tree = bpy.data.node_groups.new(tag, "CompositorNodeTree")
    tree.interface.new_socket("Image", in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    node = tree.nodes.new("CompositorNodeVecBlur")
    node.inputs['Samples'].default_value = 64
    node.inputs['Shutter'].default_value = 2.0
    node.inputs['Curved'].default_value = curved

    def connect(value, socket):
        if isinstance(value, tuple):
            upstream = tree.nodes.new("CompositorNodeRGB")
            upstream.outputs[0].default_value = value
            assert tuple(upstream.outputs[0].default_value) == value
            tree.links.new(upstream.outputs[0], node.inputs[socket])
        else:
            upstream = tree.nodes.new("CompositorNodeImage")
            upstream.image = value
            tree.links.new(upstream.outputs['Image'], node.inputs[socket])

    connect(source, 'Image')
    connect(speed, 'Speed')
    if extended is not None:
        connect(extended, 'Speed Extended')
    out = tree.nodes.new("NodeGroupOutput")
    tree.links.new(node.outputs['Image'], out.inputs['Image'])
    path = os.path.join(OUTPUT, tag + '.exr')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    result = bpy.data.images.load(path)
    values = [0.0] * (SIZE * SIZE * 4)
    result.pixels.foreach_get(values)
    return values


def compare(a, b, name, tolerance=2e-5):
    difference = max(abs(x - y) for x, y in zip(a, b))
    assert difference < tolerance, f"{name}: max difference {difference}"
    print(f"MOTION_OK {name}: {difference:.3g}", flush=True)


speed = constant_image('speed', (-36.0, -24.0, -36.0, 24.0))
# Each pixel follows a quadratic with its own velocity and acceleration. Its outer
# positions add no new information, so connecting Extended must leave the blur unchanged.
# Different pixels win the independent magnitudes within each tile in the old reduction.
inner_values = []
outer_values = []
generator = random.Random(947)
motion_fields = [(generator.randint(-20, 20), generator.randint(-20, 20),
                  generator.randint(1, 4) * 2, generator.randint(1, 4) * 2)
                 for _ in range((SIZE // 8) ** 2)]
for y in range(SIZE):
    for x in range(SIZE):
        vx, vy, ax, ay = motion_fields[(y // 8) * (SIZE // 8) + x // 8]
        inner_values.extend((ax / 2 - vx, ay / 2 - vy, -ax / 2 - vx, -ay / 2 - vy))
        outer_values.extend((2 * ax - 2 * vx, 2 * ay - 2 * vy,
                             -2 * ax - 2 * vx, -2 * ay - 2 * vy))
quadratic_speed = image('quadratic_speed', inner_values)
quadratic_outer = image('quadratic_outer', outer_values)
compare(render(quadratic_speed, None, tag='quadratic_inner'),
        render(quadratic_speed, quadratic_outer, tag='quadratic_outer'),
        'mixed quadratic trajectories remain coherent with Extended')

outer_value = (0.5, 0.25, 0.75, 0.125)
outer = constant_image('outer', outer_value)
compare(render(speed, outer, tag='outer_image'),
        render(speed, outer_value, tag='outer_constant'),
        'connected constant Extended equals an image of the same values')

speed_value = (0.5, 0.25, 0.75, 0.125)
varying = []
for y in range(SIZE):
    for x in range(SIZE):
        varying.extend((x - 64.0, y - 64.0, 64.0 - x, 64.0 - y))
varying_outer = image('varying_outer', varying)
compare(render(constant_image('small_speed', speed_value), varying_outer, tag='speed_image'),
        render(speed_value, varying_outer, tag='speed_constant'),
        'constant Speed accepts spatially varying Extended')

zero = constant_image('zero', (0.0, 0.0, 0.0, 0.0))
returning = constant_image('returning', (480.0, 0.0, -480.0, 0.0))
blur = render(zero, returning, tag='returning_path')
streak = max(blur[(y * SIZE + x) * 4]
             for y in range(62, 67) for x in range(37, 44))
assert streak > 1e-4, f"Returning trajectory was discarded: {streak}"
print(f"MOTION_OK zero endpoints retain intermediate motion: {streak:.6f}", flush=True)

compare(render(speed, None, curved=False, tag='linear_without_extended'),
        render(speed, outer, curved=False, tag='linear_with_extended'),
        'linear blur ignores Extended')

# Link convenience must respect pass availability and explicit existing links.
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
layer = scene.view_layers[0]
assert not layer.cycles.use_pass_vector_extended, "Extended default changed"
layer.use_pass_vector = True
layer.cycles.use_pass_vector_extended = True
tree = bpy.data.node_groups.new('motion_link_ux', 'CompositorNodeTree')
rl = tree.nodes.new('CompositorNodeRLayers')
rl.scene = scene
node = tree.nodes.new('CompositorNodeVecBlur')
tree.links.new(rl.outputs['Vector'], node.inputs['Speed'], handle_dynamic_sockets=True)
assert node.inputs['Speed Extended'].is_linked, "Matching Extended was not connected"
assert node.inputs['Speed Extended'].links[0].from_node == rl
custom = tree.nodes.new('CompositorNodeRGB')
tree.links.new(custom.outputs[0], node.inputs['Speed Extended'])
tree.links.new(rl.outputs['Vector'], node.inputs['Speed'], handle_dynamic_sockets=True)
assert node.inputs['Speed Extended'].links[0].from_node == custom, "Explicit Extended link replaced"
layer.cycles.use_pass_vector_extended = False
without = tree.nodes.new('CompositorNodeVecBlur')
tree.links.new(rl.outputs['Vector'], without.inputs['Speed'], handle_dynamic_sockets=True)
assert not without.inputs['Speed Extended'].is_linked, "Disabled Extended pass was linked"
print(f"VECTOR_BLUR_MOTION_OK {DEVICE}", flush=True)
