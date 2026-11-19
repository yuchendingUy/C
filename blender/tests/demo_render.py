"""Demo render with an HDRI (not part of the add-on).

blender -b scene.blend --python demo_render.py -- <hdri.hdr> <out.png> <frame> <samples> [sun_offset_deg]

Only the HDRI's orientation is chosen here: it is rotated about the vertical axis so the sun sits
sun_offset_deg to the right of the camera's view direction. Its brightness and colours are used
as they are.
"""
import math
import sys

import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
hdr, out, frame, samples = argv[0], argv[1], int(argv[2]), int(argv[3])
sun_offset = math.radians(float(argv[4]) if len(argv) > 4 else 60.0)

sc = bpy.context.scene
sc.frame_set(frame)
img = bpy.data.images.load(hdr)

# --- sun direction in the HDRI: brightest texel, mapped with Cycles' equirectangular convention
# (kernel/camera/projection.h): u = (pi - atan2(y, x)) / 2pi, v = 1 - acos(z) / pi
w, h = img.size
px = np.empty(w * h * 4, dtype=np.float32)
img.pixels.foreach_get(px)
px = px.reshape(h, w, 4)
lum = px[..., 0] * 0.2126 + px[..., 1] * 0.7152 + px[..., 2] * 0.0722
# Average over blocks first so a small bright lamp doesn't win over the sun and its glow.
bs = max(1, w // 128)
hb, wb = h // bs, w // bs
blk = lum[:hb * bs, :wb * bs].reshape(hb, bs, wb, bs).mean(axis=(1, 3))
row, col = np.unravel_index(np.argmax(blk), blk.shape)
u = (col + 0.5) / wb
v = (row + 0.5) / hb          # Blender image rows start at the bottom
sun_az = math.pi - 2 * math.pi * u
sun_el = math.pi / 2 - math.pi * (1 - v)
print("HDRI sun: azimuth %.1f deg, elevation %.1f deg, peak %.0f" % (math.degrees(sun_az), math.degrees(sun_el), blk[row, col]))

# --- camera view azimuth
cam = sc.camera
fwd = cam.matrix_world.to_3x3() @ Vector((0, 0, -1))
cam_az = math.atan2(fwd.y, fwd.x)
target_az = cam_az - sun_offset      # to the right of the view direction
alpha = sun_az - target_az           # Mapping rotates the lookup vector: lookup(R(alpha) d)
print("camera azimuth %.1f deg -> HDRI rotation %.1f deg" % (math.degrees(cam_az), math.degrees(alpha)))

world = bpy.data.worlds.new("Sunset HDRI")
sc.world = world
world.use_nodes = True
nt = world.node_tree
nt.nodes.clear()
tc = nt.nodes.new("ShaderNodeTexCoord")
mp = nt.nodes.new("ShaderNodeMapping")
mp.inputs["Rotation"].default_value = (0.0, 0.0, alpha)
env = nt.nodes.new("ShaderNodeTexEnvironment")
env.image = img
bg = nt.nodes.new("ShaderNodeBackground")
wo = nt.nodes.new("ShaderNodeOutputWorld")
nt.links.new(tc.outputs["Generated"], mp.inputs["Vector"])
nt.links.new(mp.outputs["Vector"], env.inputs["Vector"])
nt.links.new(env.outputs["Color"], bg.inputs["Color"])
nt.links.new(bg.outputs["Background"], wo.inputs["Surface"])

sc.render.engine = "CYCLES"
sc.cycles.device = "CPU"
sc.cycles.samples = samples
sc.cycles.use_denoising = True
sc.cycles.transparent_max_bounces = 64   # stacked glass
sc.render.resolution_percentage = int(argv[5]) if len(argv) > 5 else 67
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
