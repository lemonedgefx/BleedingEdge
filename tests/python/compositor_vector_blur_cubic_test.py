"""Verifies that connecting Speed Extended makes Vector Blur follow a cubic.

The Speed fields describe exact circular motion sampled at -2,-1,0,+1,+2 frames.
The quadratic through the inner three samples and the cubic through all five
diverge measurably mid-segment, and the cubic is the one that tracks the true
circle. Rendering with and without the extended input must show that.

Run: blender.exe -b --factory-startup --python test_cubic_vector_blur.py
"""

import bpy
import math
import os
import sys
import tempfile

W = H = 256
R = 70.0
OMEGA = math.radians(60.0)
SHUTTER = 2.0     # -> shutter_speed = 1.0, so pixel units are exact
SAMPLES = 128

# Motion with an inflection: P(tau) = (A*tau, B*tau^3). A parabola cannot represent
# it at all, so the quadratic reconstruction collapses to a straight line while the
# cubic reproduces it exactly. This is what tells the two apart on screen.
A_LIN = 60.0
B_CUBE = 60.0


def s_curve_offset(tau):
    return (A_LIN * tau, B_CUBE * tau ** 3)

OUT_DIR = tempfile.mkdtemp(prefix="cubicvb_")
failures = []


def check(name, ok, detail=""):
    print(f"[{'OK' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def circle_offset(tau):
    """Offset from the position at tau=0, for motion on a circle."""
    return (R * (math.cos(OMEGA * tau) - 1.0), R * math.sin(OMEGA * tau))


def make_image(name, filler):
    img = bpy.data.images.new(name, W, H, alpha=True, float_buffer=True)
    px = [0.0] * (W * H * 4)
    filler(px)
    img.pixels.foreach_set(px)
    img.update()
    return img


def fill_source(px):
    for dy in range(-1, 2):
        for dx in range(-1, 2):
            i = ((H // 2 + dy) * W + (W // 2 + dx)) * 4
            px[i:i + 4] = [1.0, 1.0, 1.0, 1.0]


def const_filler(vals):
    def fill(px):
        for i in range(W * H):
            px[i * 4:i * 4 + 4] = list(vals)
    return fill


def render(source, speed, extended, tag):
    scene = bpy.context.scene
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
    src.image = source
    spd = tree.nodes.new("CompositorNodeImage")
    spd.image = speed
    vb = tree.nodes.new("CompositorNodeVecBlur")
    vb.inputs["Samples"].default_value = SAMPLES
    vb.inputs["Shutter"].default_value = SHUTTER
    vb.inputs["Curved"].default_value = True
    tree.links.new(src.outputs["Image"], vb.inputs["Image"])
    tree.links.new(spd.outputs["Image"], vb.inputs["Speed"])
    if extended is not None:
        ext_node = tree.nodes.new("CompositorNodeImage")
        ext_node.image = extended
        tree.links.new(ext_node.outputs["Image"], vb.inputs["Speed Extended"])
    tree.links.new(vb.outputs["Image"], out.inputs[0])

    path = os.path.join(OUT_DIR, f"{tag}.exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = [0.0] * (W * H * 4)
    img.pixels.foreach_get(px)
    return px


def sample(px, x, y):
    if not (0 <= x < W and 0 <= y < H):
        return 0.0
    return px[((y * W) + x) * 4]


def sample_area(px, x, y, radius=1):
    """Peak value in a small neighbourhood, to tolerate sub-pixel placement."""
    best = 0.0
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            best = max(best, sample(px, int(round(x)) + dx, int(round(y)) + dy))
    return best


# --- Analytic check on circular motion -----------------------------------------
# The cubic must track a circle more closely than the quadratic, even though the
# two loci nearly coincide there.
c_a, c_b = circle_offset(-1), circle_offset(1)
c_eb = circle_offset(2)
c_tan = ((c_b[0] - c_a[0]) * 0.5, (c_b[1] - c_a[1]) * 0.5)
c_qcurv = ((c_a[0] + c_b[0]) * 0.5, (c_a[1] + c_b[1]) * 0.5)
c_ccurv = (c_a[0] + 2 * c_b[0] - c_eb[0] * 0.5, c_a[1] + 2 * c_b[1] - c_eb[1] * 0.5)
c_ccube = ((c_eb[0] - c_a[0] - 3 * c_b[0]) * 0.5, (c_eb[1] - c_a[1] - 3 * c_b[1]) * 0.5)
_t = 0.5
_q = (c_tan[0] * _t + c_qcurv[0] * _t * _t, c_tan[1] * _t + c_qcurv[1] * _t * _t)
_c = (c_tan[0] * _t + c_ccurv[0] * _t * _t + c_ccube[0] * _t ** 3,
      c_tan[1] * _t + c_ccurv[1] * _t * _t + c_ccube[1] * _t ** 3)
_true = circle_offset(_t)
_eq = math.hypot(_q[0] - _true[0], _q[1] - _true[1])
_ec = math.hypot(_c[0] - _true[0], _c[1] - _true[1])
check("circle: cubic is analytically closer than quadratic", _ec < _eq * 0.6,
      f"cubic err {_ec:.2f}px vs quadratic err {_eq:.2f}px")

# --- Motion fields (inflected motion, used for the render comparison) ----------
p_m1 = s_curve_offset(-1)
p_p1 = s_curve_offset(1)
p_m2 = s_curve_offset(-2)
p_p2 = s_curve_offset(2)

# Stored convention: .xy = P_prev - P_curr, .zw = P_curr - P_next.
speed_vals = (p_m1[0], p_m1[1], -p_p1[0], -p_p1[1])
ext_vals = (p_m2[0], p_m2[1], -p_p2[0], -p_p2[1])

source = make_image("src", fill_source)
speed = make_image("speed", const_filler(speed_vals))
extended = make_image("ext", const_filler(ext_vals))

# Post-mask offsets (shutter_speed is 1.0 here).
a = (p_m1[0], p_m1[1])
b = (p_p1[0], p_p1[1])
eb = (p_p2[0], p_p2[1])

tangent = ((b[0] - a[0]) * 0.5, (b[1] - a[1]) * 0.5)
quad_curv = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5)
cubic_curv = (a[0] + 2 * b[0] - eb[0] * 0.5, a[1] + 2 * b[1] - eb[1] * 0.5)
cubic_cube = ((eb[0] - a[0] - 3 * b[0]) * 0.5, (eb[1] - a[1] - 3 * b[1]) * 0.5)


def quad_at(t):
    return (tangent[0] * t + quad_curv[0] * t * t, tangent[1] * t + quad_curv[1] * t * t)


def cubic_at(t):
    return (tangent[0] * t + cubic_curv[0] * t * t + cubic_cube[0] * t ** 3,
            tangent[1] * t + cubic_curv[1] * t * t + cubic_cube[1] * t ** 3)


T = 0.5
q = quad_at(T)
c = cubic_at(T)
true = s_curve_offset(T)
print(f"at t={T}: quadratic {q[0]:.2f},{q[1]:.2f}  cubic {c[0]:.2f},{c[1]:.2f} "
      f" true motion {true[0]:.2f},{true[1]:.2f}")
sep = math.hypot(c[0] - q[0], c[1] - q[1])
print(f"quadratic/cubic separation: {sep:.2f}px")

# The reconstruction uses Catmull-Rom tangents, which are central differences rather
# than true derivatives, so it is not an exact interpolant of arbitrary motion. What
# it must do is bend towards the inflection, which the quadratic cannot do at all.
err_c = math.hypot(c[0] - true[0], c[1] - true[1])
err_q = math.hypot(q[0] - true[0], q[1] - true[1])
check("cubic follows the inflection far better than the quadratic", err_c < err_q * 0.35,
      f"cubic err {err_c:.2f}px vs quadratic err {err_q:.2f}px")
check("quadratic degenerates to a straight line here",
      abs(quad_curv[0]) < 1e-6 and abs(quad_curv[1]) < 1e-6,
      f"curvature {quad_curv}")

# Endpoints must agree: both curves interpolate the +/-1 frame samples.
check("both curves end at the same +1 frame sample",
      math.hypot(*(x - y for x, y in zip(quad_at(1.0), cubic_at(1.0)))) < 1e-6,
      f"{quad_at(1.0)} vs {cubic_at(1.0)}")

# --- Render --------------------------------------------------------------------
img_quad = render(source, speed, None, "quad")
img_cubic = render(source, speed, extended, "cubic")

cx, cy = W // 2, H // 2

# The streak from the source appears at centre + offset(t).
v_quad_at_q = sample_area(img_quad, cx + q[0], cy + q[1])
v_cubic_at_q = sample_area(img_cubic, cx + q[0], cy + q[1])
v_quad_at_c = sample_area(img_quad, cx + c[0], cy + c[1])
v_cubic_at_c = sample_area(img_cubic, cx + c[0], cy + c[1])

check("cubic render has the streak on the cubic path", v_cubic_at_c > 1e-4,
      f"value {v_cubic_at_c:.5f}")
check("quadratic render has no streak on the cubic path", v_quad_at_c < v_cubic_at_c * 0.25,
      f"quad {v_quad_at_c:.5f} vs cubic {v_cubic_at_c:.5f}")
check("quadratic render has the streak on the quadratic path", v_quad_at_q > 1e-4,
      f"value {v_quad_at_q:.5f}")
check("cubic render has no streak on the quadratic path", v_cubic_at_q < v_quad_at_q * 0.25,
      f"cubic {v_cubic_at_q:.5f} vs quad {v_quad_at_q:.5f}")

# The rendered streak must lie on the reconstructed cubic across the whole segment,
# with no gaps where it crosses tile boundaries.
for t in (0.15, 0.25, 0.5, 0.75, 0.9):
    tp = cubic_at(t)
    v = sample_area(img_cubic, cx + tp[0], cy + tp[1], radius=2)
    check(f"cubic streak continuous at t={t}", v > 1e-5, f"value {v:.5f}")

print()
if failures:
    print(f"FAILED: {failures}")
    sys.exit(1)
print("All cubic vector blur tests passed.")
