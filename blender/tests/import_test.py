"""Headless import test: blender -b --factory-startup --python import_test.py -- <scene.screc> <out.blend>"""
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import scenerec_importer
from scenerec_importer import importer

scenerec_importer.register()
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
importer.import_recording(bpy.context, src)
sc = bpy.context.scene
print("SCENE fps", sc.render.fps, "frames", sc.frame_start, sc.frame_end, "res", sc.render.resolution_x, sc.render.resolution_y, "engine", sc.render.engine)
deps = bpy.context.evaluated_depsgraph_get()
for f in (1, sc.frame_end // 2, sc.frame_end):
    sc.frame_set(f)
    deps = bpy.context.evaluated_depsgraph_get()
    stats = []
    for o in sc.objects:
        if o.type != "MESH":
            continue
        eo = o.evaluated_get(deps)
        hidden = o.hide_render
        me = eo.to_mesh()
        stats.append((o.name, len(me.vertices), len(me.polygons), "hidden" if hidden else "", tuple(round(v, 2) for v in eo.matrix_world.translation)))
        eo.to_mesh_clear()
    cam = sc.camera
    print("FRAME", f, "camera", tuple(round(v, 2) for v in cam.matrix_world.translation), "lens", round(cam.data.lens, 2))
    for s in sorted(stats):
        print("   ", s)
bpy.ops.wm.save_as_mainfile(filepath=out)
print("SAVED", out)
