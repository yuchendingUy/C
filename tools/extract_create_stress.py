#!/usr/bin/env python3
"""从机械动力源码 AllBlocks.java 提取每个方块的默认应力值（CStress.setImpact / setCapacity / setNoImpact）。
用法：extract_create_stress.py <ref/create 目录> <输出 stress.txt>"""
import re, sys

base = sys.argv[1] + '/src/main/java/com/simibubi/create/'
src = open(base + 'AllBlocks.java', encoding='utf-8').read()
bt = open(base + 'foundation/data/BuilderTransformers.java', encoding='utf-8').read()

def stress_of(body):
    """返回 (影响, 容量)，没写为 None"""
    imp = re.search(r'CStress\.setImpact\(([0-9.]+)\)', body)
    cap = re.search(r'CStress\.setCapacity\(([0-9.]+)\)', body)
    imp_v = 0.0 if 'CStress.setNoImpact()' in body else (float(imp.group(1)) if imp else None)
    return imp_v, (float(cap.group(1)) if cap else None)

# BuilderTransformers 里间接设置应力的模板（encasedShaft、mechanicalPiston 等）
transformers = {}
parts = re.split(r'\n\tpublic static ', bt)
for part in parts[1:]:
    m = re.search(r'([a-zA-Z0-9_]+)\(', part)
    if m:
        transformers[m.group(1)] = stress_of(part)
out = []
# 每个方块：REGISTRATE.block("名字", ...) 到 .register(); 之间
for m in re.finditer(r'\.block\("([a-z0-9_]+)"(.*?)\.register\(\)', src, re.S):
    name, body = m.group(1), m.group(2)
    imp_v, cap_v = stress_of(body)
    for t in re.findall(r'BuilderTransformers\.([a-zA-Z0-9_]+)\(', body):
        ti, tc = transformers.get(t, (None, None))
        if imp_v is None: imp_v = ti
        if cap_v is None: cap_v = tc
    if imp_v is None and cap_v is None:
        continue
    out.append((name, imp_v, cap_v))

with open(sys.argv[2], 'w', encoding='utf-8') as f:
    f.write('# 机械动力方块默认应力值（由 tools/extract_create_stress.py 从 AllBlocks.java 提取）\n')
    f.write('# S <方块名> <应力影响（每 RPM）> <应力容量（每 RPM）>，未设置写 -\n')
    for name, i, c in out:
        f.write('S create:%s %s %s\n' % (name, '-' if i is None else repr(i), '-' if c is None else repr(c)))
print(len(out), 'blocks')
