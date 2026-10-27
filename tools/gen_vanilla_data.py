#!/usr/bin/env python3
"""
从 PrismarineJS minecraft-data 生成 mcc 的注册表数据文件。

用法: gen_vanilla_data.py <minecraft-data/data 目录> <输出目录>

输出:
  blocks.txt      方块与方块状态（与 NeoForge 导出工具输出同一格式）
  registries.txt  配置阶段要同步的数据驱动注册表条目（顺序即网络编号）

blocks.txt 格式（每行一条，空格分隔）:
  B <命名空间:名称> <首个状态编号> <状态数> <默认状态编号> <硬度> <爆炸抗性> <标志> <发光> <滤光> <属性数>
  P <属性名> <取值数> <取值1> <取值2> ...
  标志位: 1=透明 2=完整碰撞箱 4=可挖掘
  状态编号规则: 属性按出现顺序排列，最后一个属性变化最快（与原版 StateDefinition 一致）
"""
import json
import os
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    data, out = sys.argv[1], sys.argv[2]
    paths = json.load(open(os.path.join(data, 'dataPaths.json')))['pc']['1.21.1']
    blocks = json.load(open(os.path.join(data, paths['blocks'], 'blocks.json')))
    login = json.load(open(os.path.join(data, paths['loginPacket'], 'loginPacket.json')))
    os.makedirs(out, exist_ok=True)

    with open(os.path.join(out, 'blocks.txt'), 'w') as f:
        f.write('# mcc 方块注册表 1.21.1 原版（由 gen_vanilla_data.py 生成）\n')
        expect = 0
        for b in blocks:
            assert b['minStateId'] == expect, b['name']
            count = b['maxStateId'] - b['minStateId'] + 1
            flags = (1 if b['transparent'] else 0) | (2 if b['boundingBox'] == 'block' else 0) | (4 if b['diggable'] else 0)
            props = b['states']
            prod = 1
            for p in props:
                prod *= p['num_values']
            assert prod == count, b['name']
            f.write('B minecraft:%s %d %d %d %g %g %d %d %d %d\n' % (
                b['name'], b['minStateId'], count, b['defaultState'], b['hardness'] if b['hardness'] is not None else -1,
                b['resistance'], flags, b['emitLight'], b['filterLight'], len(props)))
            for p in props:
                if p['type'] == 'bool':
                    vals = ['true', 'false']
                else:
                    vals = p['values']
                assert len(vals) == p['num_values']
                f.write('P %s %d %s\n' % (p['name'], len(vals), ' '.join(vals)))
            expect += count
        print('方块 %d 种，状态 %d 个' % (len(blocks), expect))

    with open(os.path.join(out, 'registries.txt'), 'w') as f:
        f.write('# 配置阶段同步的注册表（条目顺序即网络编号）\n')
        codec = login['dimensionCodec']
        for reg, v in codec.items():
            keys = [e['key'] for e in v['entries']]
            f.write('R %s %d\n' % (reg, len(keys)))
            for k in keys:
                f.write('E %s\n' % k)
        print('注册表 %d 个' % len(codec))


if __name__ == '__main__':
    main()
