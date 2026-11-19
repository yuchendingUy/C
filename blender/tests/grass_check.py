"""Close-up of a grass block side: blender -b --factory-startup --python grass_check.py -- scene.screc out.png step"""
import os, sys
import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out, step = argv[0], argv[1], float(argv[2])
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from scenerec_importer import importer, geom, screc
geom.COPLANAR_STEP = step
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
b = importer.import_recording(bpy.context, src)
rec = b.rec
# find a grass_block_side quad facing +X/-X/+Z/-Z near the start camera
tid = [k for k, v in rec.textures.items() if v["name"].endswith("atlas/blocks.png")][0]
lk = rec.sprite_lookup(tid)
best = None
for mid, r in rec.retained.items():
    st = rec.states[r["draws"][0][1]]
    if st["render_type"] != "cutout_mipped":
        continue
    dec = rec.formats[r["format"]].decode(r["data"], r["vertex_count"])
    q = dec["positions"].reshape(-1, 4, 3)
    names = np.array(lk.names)[lk.sprite_of(dec["uv"].reshape(-1, 4, 2).mean(axis=1))]
    sel = np.where(names == "minecraft:block/grass_block_side")[0]
    _, _, _, tr, m9 = r["draws"][0]
    for i in sel:
        w = geom.transform_points(q[i], m9, tr, b.origin)
        c = w.mean(axis=0)
        n = np.cross(w[1] - w[0], w[2] - w[0]); n /= np.linalg.norm(n)
        if abs(n[2]) > 0.1:
            continue
        d = np.linalg.norm(c)
        if best is None or d < best[0]:
            best = (d, c, n)
_, c, n = best
cam = bpy.context.scene.camera
cam.animation_data_clear()
cam.data.animation_data_clear()
cam.location = Vector(c + n * 2.5)
cam.rotation_mode = "QUATERNION"
cam.rotation_quaternion = Vector(-n).to_track_quat("-Z", "Y")
cam.data.lens = 30
sc = bpy.context.scene
sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
sun.data.energy = 3
sun.rotation_euler = (0.9, 0.2, 0.6)
sc.collection.objects.link(sun)
sc.world = sc.world or bpy.data.worlds.new("W")
sc.world.use_nodes = True
sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0.9, 0.2, 0.9, 1)  # magenta: holes show up
sc.cycles.device = "CPU"
sc.cycles.samples = 16
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 480, 270, 100
sc.frame_set(50)
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out, "face center", c, "normal", n)
