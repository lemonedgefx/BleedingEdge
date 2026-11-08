"""Functional test for curved (quadratic) interpolation in the Vector Blur node.

Constructs a constant Speed field describing a symmetric arc:
    offset to previous position = (-L, -h)
    offset to next position     = (+L, -h)
so the LINEAR path from a bright pixel is an inverted V (drops immediately on both
sides), while the CURVED path is a parabola leaving the centre horizontally.

Run: blender.exe -b --factory-startup --python test_curved_vector_blur.py
"""

import bpy
import os
import sys
import tempfile

W = H = 256
L = 60.0   # horizontal reach in pixels (spans ~2 tiles of 32)
HH = 40.0  # vertical drop at the endpoints
SHUTTER = 2.0  # -> shutter_speed = 1.0, so pixel units are exact
SAMPLES = 64

OUT_DIR = tempfile.mkdtemp(prefix="curvedvb_")
failures = []


def check(name, ok, detail=""):
    print(f"[{'OK' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def make_image(name, filler):
    img = bpy.data.images.new(name, W, H, alpha=True, float_buffer=True)
    px = [0.0] * (W * H * 4)
    filler(px)
    img.pixels.foreach_set(px)
    img.update()
    return img


def fill_source(px):
    """A small bright block at the centre."""
    for dy in range(-1, 2):
        for dx in range(-1, 2):
            i = ((H // 2 + dy) * W + (W // 2 + dx)) * 4
            px[i:i + 4] = [1.0, 1.0, 1.0, 1.0]


def make_speed_filler(prev_xy, next_zw):
    def fill(px):
        for i in range(W * H):
            px[i * 4:i * 4 + 4] = [prev_xy[0], prev_xy[1], next_zw[0], next_zw[1]]
    return fill


DEVICE = os.environ.get("VB_DEVICE", "CPU")


def render_vector_blur(source_img, speed_img, curved, tag):
    scene = bpy.context.scene
    scene.render.compositor_device = DEVICE
    scene.render.resolution_x = W
    scene.render.resolution_y = H
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.view_settings.view_transform = 'Raw'

    tree = bpy.data.node_groups.new(f"vb_{tag}", "CompositorNodeTree")
    tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    scene.compositing_node_group = tree
    tree.nodes.clear()

    out = tree.nodes.new("NodeGroupOutput")
    src = tree.nodes.new("CompositorNodeImage")
    src.image = source_img
    spd = tree.nodes.new("CompositorNodeImage")
    spd.image = speed_img
    vb = tree.nodes.new("CompositorNodeVecBlur")
    vb.inputs["Samples"].default_value = SAMPLES
    vb.inputs["Shutter"].default_value = SHUTTER
    vb.inputs["Curved"].default_value = curved

    tree.links.new(src.outputs["Image"], vb.inputs["Image"])
    tree.links.new(spd.outputs["Image"], vb.inputs["Speed"])
    tree.links.new(vb.outputs["Image"], out.inputs[0])

    path = os.path.join(OUT_DIR, f"{tag}.exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)

    result = bpy.data.images.load(path)
    px = [0.0] * (W * H * 4)
    result.pixels.foreach_get(px)
    return px


def sample(px, x, y):
    """Luminance-ish value at pixel (x, y); image origin is bottom-left."""
    return px[((y * W) + x) * 4]


source = make_image("source", fill_source)

# --- Case 1: symmetric arc -----------------------------------------------------
arc = make_image("speed_arc", make_speed_filler((-L, -HH), (-L, HH)))
lin = render_vector_blur(source, arc, False, "arc_linear")
crv = render_vector_blur(source, arc, True, "arc_curved")

cx, cy = W // 2, H // 2


def streak_peak_row(px, x):
    """Row of the brightest pixel in column x, and its value."""
    best_y, best_v = -1, 0.0
    for y in range(H):
        v = sample(px, x, y)
        if v > best_v:
            best_y, best_v = y, v
    return best_y, best_v


# The streak traced from the centre follows offset(t) with x = L*t, so at a
# horizontal distance dx the vertical drop is HH*(dx/L) when linear and
# HH*(dx/L)^2 when curved. Verifying the peak row against both predictions is a
# direct test of the path shape.
for dx in (16, 24, 32):
    t = dx / L
    predicted_linear = cy - HH * t
    predicted_curved = cy - HH * t * t

    lin_y, lin_v = streak_peak_row(lin, cx + dx)
    crv_y, crv_v = streak_peak_row(crv, cx + dx)

    check(f"linear streak follows the chord at +{dx}px",
          lin_v > 1e-5 and abs(lin_y - predicted_linear) <= 2.0,
          f"peak row {lin_y} vs predicted {predicted_linear:.1f} (value {lin_v:.5f})")
    check(f"curved streak follows the parabola at +{dx}px",
          crv_v > 1e-5 and abs(crv_y - predicted_curved) <= 2.0,
          f"peak row {crv_y} vs predicted {predicted_curved:.1f} (value {crv_v:.5f})")

# Symmetry: a sign error in the tangent would break left/right balance.
for dx in (16, 32):
    left_y, left_v = streak_peak_row(crv, cx - dx)
    right_y, right_v = streak_peak_row(crv, cx + dx)
    check(f"curved symmetric at +/-{dx}px",
          left_y == right_y and abs(left_v - right_v) <= 0.05 * max(left_v, right_v, 1e-4),
          f"rows {left_y}/{right_y}, values {left_v:.5f}/{right_v:.5f}")

# Continuity across tile boundaries (catches a missing dilation widening).
gaps = [dx for dx in range(4, 60, 4) if streak_peak_row(crv, cx + dx)[1] <= 1e-5]
check("curved streak continuous across tiles", not gaps, f"empty at dx={gaps}")

# --- Case 2: constant velocity must be identical to linear ---------------------
# For motion to the right at L px/frame the pass stores
#   .xy = P_prev - P_curr = (-L, 0)   and   .zw = P_curr - P_next = (-L, 0).
straight = make_image("speed_straight", make_speed_filler((-L, 0.0), (-L, 0.0)))
lin2 = render_vector_blur(source, straight, False, "straight_linear")
crv2 = render_vector_blur(source, straight, True, "straight_curved")

max_diff = max(abs(a - b) for a, b in zip(lin2, crv2))
check("constant velocity: curved == linear", max_diff < 1e-5, f"max diff {max_diff:.3e}")

print()
if failures:
    print(f"FAILED ({DEVICE}): {failures}")
    sys.exit(1)
print(f"All curved vector blur tests passed ({DEVICE}).")
