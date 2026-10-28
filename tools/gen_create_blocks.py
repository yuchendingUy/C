#!/usr/bin/env python3
"""
生成机械动力方块注册表 data/create/blocks.txt 与碰撞形状 data/create/shapes.txt。

方块属性照机械动力源码里各方块类的 createBlockStateDefinition 手写在下面的 BLOCKS 表里；
状态编号排法与原版一致：属性按名字排序，最后一个属性变化最快。
每个文件的状态编号、形状编号都从 0 写起，载入时接在原版之后。

用法：gen_create_blocks.py <输出目录，默认 data/create>
"""
import itertools, os, sys

AXIS = ('axis', ['x', 'y', 'z'])
HAXIS = ('axis', ['x', 'z'])                       # BlockStateProperties.HORIZONTAL_AXIS
FACING = ('facing', ['north', 'east', 'south', 'west', 'up', 'down'])
WATERLOGGED = ('waterlogged', ['true', 'false'])
POWERED = ('powered', ['true', 'false'])
def boolean(name): return (name, ['true', 'false'])

# 原版标志位（与 blocks.txt 一致）：1 透明（非整块），2 整块碰撞箱，4 可挖
TRANSPARENT, FULL_BOX, DIGGABLE = 1, 2, 4

# ---- 形状（体素坐标 0~16，照 AllShapes） ----
def box(*v): return [tuple(x / 16 for x in v)]
FULL = box(0, 0, 0, 16, 16, 16)

def for_axis(shape_y):
    """AllShapes forAxis：以 Y 轴为基准的形状转到 X / Z 轴"""
    def f(axis):
        out = []
        for (x0, y0, z0, x1, y1, z1) in shape_y:
            if axis == 'y': out.append((x0, y0, z0, x1, y1, z1))
            elif axis == 'x': out.append((y0, x0, z0, y1, x1, z1))
            else: out.append((x0, z0, y0, x1, z1, y1))
        return out
    return f

def for_directional(shape_up):
    """AllShapes forDirectional：朝上为基准的形状转到六个朝向"""
    def f(facing):
        out = []
        for (x0, y0, z0, x1, y1, z1) in shape_up:
            if facing == 'up': b = (x0, y0, z0, x1, y1, z1)
            elif facing == 'down': b = (x0, 1 - y1, z0, x1, 1 - y0, z1)
            elif facing == 'north': b = (x0, z0, 1 - y1, x1, z1, 1 - y0)
            elif facing == 'south': b = (x0, z0, y0, x1, z1, y1)
            elif facing == 'west': b = (1 - y1, x0, z0, 1 - y0, x1, z1)
            else: b = (y0, x0, z0, y1, x1, z1)
            out.append(b)
        return out
    return f

SIX_VOXEL_POLE = for_axis(box(5, 0, 5, 11, 16, 11))
SMALL_GEAR = for_axis(box(2, 6, 2, 14, 10, 14) + box(5, 0, 5, 11, 16, 11))
LARGE_GEAR = for_axis(box(0, 6, 0, 16, 10, 16) + box(5, 0, 5, 11, 16, 11))
MOTOR_BLOCK = for_directional(box(3, 0, 3, 13, 14, 13))
SPEED_CONTROLLER = box(0, 0, 0, 16, 4, 16) + box(1, 1, 1, 15, 13, 15)

# ---- 方块表 ----
# (名字, 属性列表, 默认值, 硬度, 抗爆, 标志, 发光, 滤光, 形状函数(状态字典) -> 碰撞箱列表)
# 硬度：机械动力多数方块 initialProperties(SharedProperties::stone) = 安山岩 1.5 / 6
# 滤光：整块且不透光 15；noOcclusion 的整块 1；非整块 0
def shaft_like(name, shape):
    return (name, [AXIS, WATERLOGGED], {'axis': 'y', 'waterlogged': 'false'}, 1.5, 6, TRANSPARENT | DIGGABLE, 0, 0,
            lambda s: shape(s['axis']))

BLOCKS = [
    # content/kinetics/simpleRelays：ShaftBlock、CogWheelBlock（AbstractShaftBlock：AXIS + WATERLOGGED）
    shaft_like('create:shaft', SIX_VOXEL_POLE),
    shaft_like('create:cogwheel', SMALL_GEAR),
    shaft_like('create:large_cogwheel', LARGE_GEAR),
    # EncasedShaftBlock（AbstractEncasedShaftBlock：AXIS）
    ('create:andesite_encased_shaft', [AXIS], {'axis': 'y'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    ('create:brass_encased_shaft', [AXIS], {'axis': 'y'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    # EncasedCogwheelBlock：AXIS + TOP_SHAFT + BOTTOM_SHAFT
    *[(f'create:{m}_encased_{k}cogwheel', [AXIS, boolean('bottom_shaft'), boolean('top_shaft')],
       {'axis': 'y', 'bottom_shaft': 'false', 'top_shaft': 'false'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL)
      for k in ('', 'large_') for m in ('andesite', 'brass')],
    # GearboxBlock（RotatedPillarKineticBlock：AXIS）
    ('create:gearbox', [AXIS], {'axis': 'y'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    # ClutchBlock / GearshiftBlock：AXIS + POWERED
    ('create:clutch', [AXIS, POWERED], {'axis': 'y', 'powered': 'false'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    ('create:gearshift', [AXIS, POWERED], {'axis': 'y', 'powered': 'false'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    # ChainDriveBlock：AXIS + PART + AXIS_ALONG_FIRST_COORDINATE（默认取第一个值 true）
    ('create:encased_chain_drive', [AXIS, boolean('axis_along_first'), ('part', ['start', 'middle', 'end', 'none'])],
     {'axis': 'y', 'axis_along_first': 'true', 'part': 'none'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1, lambda s: FULL),
    # ChainGearshiftBlock：ChainDriveBlock + POWERED
    ('create:adjustable_chain_gearshift',
     [AXIS, boolean('axis_along_first'), ('part', ['start', 'middle', 'end', 'none']), POWERED],
     {'axis': 'y', 'axis_along_first': 'true', 'part': 'none', 'powered': 'false'}, 1.5, 6, FULL_BOX | DIGGABLE, 0, 1,
     lambda s: FULL),
    # CreativeMotorBlock（DirectionalKineticBlock：FACING，默认取第一个值 north）
    ('create:creative_motor', [FACING], {'facing': 'north'}, 1.5, 6, TRANSPARENT | DIGGABLE, 0, 0,
     lambda s: MOTOR_BLOCK(s['facing'])),
    # SpeedControllerBlock（HorizontalAxisKineticBlock：HORIZONTAL_AXIS，默认 x）
    ('create:rotation_speed_controller', [HAXIS], {'axis': 'x'}, 1.5, 6, TRANSPARENT | DIGGABLE, 0, 0,
     lambda s: SPEED_CONTROLLER),
]


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else 'data/create'
    os.makedirs(out_dir, exist_ok=True)
    blines = ['# mcc 方块注册表：机械动力（由 tools/gen_create_blocks.py 生成，状态编号从 0 写起，载入时接在原版之后）']
    slines = ['# mcc 方块碰撞形状：机械动力（由 tools/gen_create_blocks.py 生成，形状编号从 0 写起）']
    clines = []
    shape_ids = {}
    state = 0
    for name, props, default, hard, res, flags, emit, filt, shape in BLOCKS:
        props = sorted(props, key=lambda p: p[0])
        combos = list(itertools.product(*[p[1] for p in props]))
        dflt = tuple(default[p[0]] for p in props)
        blines.append(f'B {name} {state} {len(combos)} {state + combos.index(dflt)} {hard} {res} {flags} {emit} {filt} {len(props)}')
        for pname, vals in props:
            blines.append(f'P {pname} {len(vals)} ' + ' '.join(vals))
        ids = []
        for combo in combos:
            boxes = tuple(tuple(round(v, 6) for v in b) for b in shape(dict(zip([p[0] for p in props], combo))))
            if boxes not in shape_ids:
                shape_ids[boxes] = len(shape_ids)
                slines.append(f'S {shape_ids[boxes]} {len(boxes)} ' + ' '.join(f'{v:g}' for b in boxes for v in b))
            ids.append(shape_ids[boxes])
        if len(set(ids)) == 1:
            clines.append(f'C {name} 1 {ids[0]}')
        else:
            clines.append(f'C {name} {len(ids)} ' + ' '.join(map(str, ids)))
        state += len(combos)
    open(os.path.join(out_dir, 'blocks.txt'), 'w').write('\n'.join(blines) + '\n')
    open(os.path.join(out_dir, 'shapes.txt'), 'w').write('\n'.join(slines + clines) + '\n')
    print(f'{len(BLOCKS)} 种方块，{state} 个状态，{len(shape_ids)} 种形状')


if __name__ == '__main__':
    main()
