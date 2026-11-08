"""Verifies the Cycles Vector Extended pass against analytically known motion.

An object is keyframed onto a circle at every integer frame and rendered with an
orthographic camera, so the screen space offsets to frames -2, -1, +1 and +2 are
exactly computable. Checks that:
  * the Vector pass still holds the +/-1 frame offsets,
  * the Vector Extended pass holds the +/-2 frame offsets,
  * enabling the extended pass does not alter the Vector pass.

Run: blender.exe -b --factory-startup --python test_extended_motion_pass.py
"""

import bpy
import glob
import math
import os
import sys
import tempfile

RES = 256
ORTHO_SCALE = 10.0
RADIUS = 2.0
OMEGA = math.radians(25.0)   # per frame
FRAME = 5
PX_PER_UNIT = RES / ORTHO_SCALE
TOL = 0.75                   # pixels

OUT_DIR = tempfile.mkdtemp(prefix="extmotion_")
failures = []


def check(name, ok, detail=""):
    print(f"[{'OK' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def circle_pos(frame):
    angle = OMEGA * frame
    return (RADIUS * math.cos(angle), RADIUS * math.sin(angle))


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 8
    scene.render.resolution_x = RES
    scene.render.resolution_y = RES
    scene.render.resolution_percentage = 100
    scene.render.use_motion_blur = False
    scene.frame_set(FRAME)

    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = 'ORTHO'
    cam_data.ortho_scale = ORTHO_SCALE
    cam = bpy.data.objects.new("cam", cam_data)
    cam.location = (0.0, 0.0, 10.0)
    bpy.context.collection.objects.link(cam)
    scene.camera = cam

    mesh = bpy.data.meshes.new("marker")
    s = 0.6
    mesh.from_pydata([(-s, -s, 0), (s, -s, 0), (s, s, 0), (-s, s, 0)], [], [(0, 1, 2, 3)])
    mesh.validate()
    obj = bpy.data.objects.new("marker", mesh)
    bpy.context.collection.objects.link(obj)

    for f in range(1, 11):
        x, y = circle_pos(f)
        obj.location = (x, y, 0.0)
        obj.keyframe_insert("location", frame=f)
    return scene


def render_pass(scene, use_extended, pass_name, tag, x, y):
    """Render with the given pass routed to the output, and sample it at (x, y).

    Returns (value_tuple, available_socket_names)."""
    view_layer = scene.view_layers[0]
    view_layer.use_pass_vector = True
    view_layer.cycles.use_pass_vector_extended = use_extended

    tree = bpy.data.node_groups.new(f"cmp_{tag}", "CompositorNodeTree")
    tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    scene.compositing_node_group = tree
    tree.nodes.clear()

    rlayers = tree.nodes.new("CompositorNodeRLayers")
    rlayers.scene = scene
    out = tree.nodes.new("NodeGroupOutput")

    available = [s.name for s in rlayers.outputs]
    if pass_name not in available:
        return None, available
    tree.links.new(rlayers.outputs[pass_name], out.inputs[0])

    path = os.path.join(OUT_DIR, f"{tag}.exr")
    scene.render.filepath = path
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.view_settings.view_transform = 'Raw'
    bpy.ops.render.render(write_still=True)

    img = bpy.data.images.load(path)
    px = [0.0] * (RES * RES * 4)
    img.pixels.foreach_get(px)
    i = ((y * RES) + x) * 4
    return tuple(px[i:i + 4]), available


scene = build_scene()

p0 = circle_pos(FRAME)
offsets = {d: circle_pos(FRAME + d) for d in (-2, -1, 1, 2)}


def to_px(target):
    return ((target[0] - p0[0]) * PX_PER_UNIT, (target[1] - p0[1]) * PX_PER_UNIT)


# Blender's convention: .xy = P_prev - P_curr, .zw = P_curr - P_next.
exp_vector = to_px(offsets[-1]) + tuple(-v for v in to_px(offsets[1]))
exp_extended = to_px(offsets[-2]) + tuple(-v for v in to_px(offsets[2]))

cx = int(RES * 0.5 + p0[0] * PX_PER_UNIT)
cy = int(RES * 0.5 + p0[1] * PX_PER_UNIT)
print(f"object centre pixel: ({cx}, {cy})")
print("expected Vector          :", [f"{v:.3f}" for v in exp_vector])
print("expected Vector Extended :", [f"{v:.3f}" for v in exp_extended])
print()

# --- Render without the extended pass (baseline) -------------------------------
base_vector, avail_base = render_pass(scene, False, "Vector", "base_vector", cx, cy)
check("baseline Vector pass renders", base_vector is not None, str(avail_base))
check("Vector Extended absent when disabled", "Vector Extended" not in avail_base,
      f"sockets: {avail_base}")

# --- Render with the extended pass ---------------------------------------------
ext_vector, avail_ext = render_pass(scene, True, "Vector", "ext_vector", cx, cy)
check("Vector Extended socket appears when enabled", "Vector Extended" in avail_ext,
      f"sockets: {avail_ext}")
ext_extended, _ = render_pass(scene, True, "Vector Extended", "ext_extended", cx, cy)
check("Vector Extended pass renders", ext_extended is not None, str(ext_extended))

if ext_vector and ext_extended and base_vector:
    err_v = max(abs(a - b) for a, b in zip(ext_vector, exp_vector))
    check("Vector pass matches +/-1 frame offsets", err_v < TOL,
          f"got {[f'{v:.3f}' for v in ext_vector]} err {err_v:.3f}px")

    err_e = max(abs(a - b) for a, b in zip(ext_extended, exp_extended))
    check("Vector Extended matches +/-2 frame offsets", err_e < TOL,
          f"got {[f'{v:.3f}' for v in ext_extended]} err {err_e:.3f}px")

    err_regress = max(abs(a - b) for a, b in zip(base_vector, ext_vector))
    check("Vector pass unchanged by enabling extended", err_regress < 1e-4,
          f"max diff {err_regress:.3e}")

    # The extended offsets must be genuinely larger than the +/-1 ones: this is
    # what proves the scene was really sampled two frames out.
    ratio = math.hypot(*ext_extended[:2]) / max(math.hypot(*ext_vector[:2]), 1e-6)
    check("extended offsets span a longer arc", 1.7 < ratio < 2.3, f"ratio {ratio:.3f}")

print()
if failures:
    print(f"FAILED: {failures}")
    sys.exit(1)
print("All extended motion pass tests passed.")
