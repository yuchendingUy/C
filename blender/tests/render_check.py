"""Verification render (not part of the add-on): blender -b file.blend --python render_check.py -- out.png frame"""
import sys
import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
out, frame = argv[0], int(argv[1])
sc = bpy.context.scene
sun_data = bpy.data.lights.new("TestSun", "SUN")
sun_data.energy = 3.0
sun = bpy.data.objects.new("TestSun", sun_data)
sun.rotation_euler = (0.9, 0.2, 0.6)
sc.collection.objects.link(sun)
world = sc.world or bpy.data.worlds.new("World")
sc.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get("Background")
if bg:
    bg.inputs[0].default_value = (0.5, 0.65, 0.9, 1)
    bg.inputs[1].default_value = 0.8
sc.render.engine = "CYCLES"
sc.cycles.device = "CPU"
sc.cycles.samples = int(argv[2]) if len(argv) > 2 else 16
sc.render.resolution_percentage = 25
sc.frame_set(frame)
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
