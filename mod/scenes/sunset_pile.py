"""Generates autotest commands for the 'sunset pile' demo scene (superflat world, surface y = -60)."""
import math
import random

rng = random.Random(20261119)
G = -60  # first air block above the superflat grass

cmds = ["time set 12500", "weather clear", "gamerule doDaylightCycle false", "gamerule doMobSpawning false",
        "kill @e[type=!player]"]

# uneven mound in the middle so falling blocks tumble instead of stacking in columns
mound = []
for x in range(-2, 3):
    for z in range(-2, 3):
        h = max(0, 2 - int(math.hypot(x, z) + rng.random() * 0.8))
        for y in range(h + 1):
            if x * x + z * z <= 5:
                mound.append((x, G + y, z, rng.choice(["stone_bricks", "mossy_stone_bricks", "cobblestone", "mossy_cobblestone"])))
for x, y, z, b in mound:
    cmds.append(f"setblock {x} {y} {z} minecraft:{b}")
cmds.append(f"fill 3 {G} 1 6 {G} 1 minecraft:oak_log[axis=x]")
cmds.append(f"setblock -4 {G} -3 minecraft:spruce_log")
cmds.append(f"setblock -4 {G + 1} -3 minecraft:spruce_log")

# rigid multi-block shapes, assembled as single bodies, high above
shapes = [
    [(0, 0, 0), (1, 0, 0), (2, 0, 0), (2, 1, 0), (2, 2, 0)],             # L
    [(0, 0, 0), (0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 2, 1)],             # twisted
    [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (0, 0, 1), (1, 1, 1)],  # glass cage
]
shape_blocks = ["oak_planks", "spruce_planks", "glass"]
for i, sh in enumerate(shapes):
    ox, oy, oz = -3 + i * 3, G + 28 + i * 3, -1 + (i % 2) * 2
    for dx, dy, dz in sh:
        cmds.append(f"setblock {ox + dx} {oy + dy} {oz + dz} minecraft:{shape_blocks[i]}")
    cmds.append(f"sable assemble area {ox - 1} {oy - 1} {oz - 1} {ox + 3} {oy + 3} {oz + 3}")

# random cloud of single blocks; every block becomes its own physics body
palette = (["glass"] * 6 + ["white_stained_glass", "light_blue_stained_glass", "orange_stained_glass",
           "magenta_stained_glass", "cyan_stained_glass", "red_stained_glass", "yellow_stained_glass"] * 2
           + ["oak_planks", "stone_bricks", "copper_block", "gold_block", "quartz_block", "sea_lantern",
              "glowstone", "amethyst_block", "white_concrete", "terracotta", "polished_andesite", "crying_obsidian"])
y0, y1 = G + 8, G + 24
for x in range(-4, 5):
    for z in range(-4, 5):
        for y in range(y0, y1 + 1):
            if rng.random() < 0.09:
                cmds.append(f"setblock {x} {y} {z} minecraft:{rng.choice(palette)}")
cmds.append(f"sable assemble shatter area -4 {y0} -4 4 {y1} 4")

# scattered trees around the field so the background isn't empty
for i in range(26):
    ang = rng.random() * 2 * math.pi
    r = 14 + rng.random() * 40
    x, z = int(r * math.cos(ang)), int(r * math.sin(ang))
    if -16 < x < -6 and -14 < z < -4:  # keep the camera spot free
        continue
    cmds.append(f"place feature minecraft:{rng.choice(['oak', 'fancy_oak', 'birch', 'oak'])} {x} {G} {z}")

# camera: player on a 3-block pillar south-west of the pile, looking at it
px, pz = -11, -9
cmds.append(f"fill {px} {G} {pz} {px} {G + 2} {pz} minecraft:stone")
tx, ty, tz = 0.5, G + 1.5, 0.5
eye = G + 3 + 1.62
dx, dz = tx - (px + 0.5), tz - (pz + 0.5)
yaw = math.degrees(math.atan2(-dx, dz))
pitch = math.degrees(math.atan2(eye - ty, math.hypot(dx, dz)))
cmds.append(f"tp @a {px + 0.5} {G + 3} {pz + 0.5} {yaw:.1f} {pitch:.1f}")

# second wave during the recording: glass and lights dropping onto the pile
start = []
for i in range(14):
    x, z = rng.randint(-2, 2), rng.randint(-2, 2)
    y = G + 7 + i
    start.append(f"setblock {x} {y} {z} minecraft:{rng.choice(['glass', 'light_blue_stained_glass', 'orange_stained_glass', 'sea_lantern', 'glowstone'])}")
start.append(f"sable assemble shatter area -2 {G + 7} -2 2 {G + 21} 2")

print(";".join(cmds))
print(";".join(start))
