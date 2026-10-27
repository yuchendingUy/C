/*
 * 方块状态 → 碰撞体（对应 Sable 按方块状态烘焙碰撞体）
 *
 * 碰撞形状来自原版数据（tools/gen_vanilla_data.py 生成的 shapes.txt），
 * 物理属性（质量、摩擦倍率、弹性、浮力体积）来自 phys_props.txt，未列出的用默认值。
 * 碰撞体按 形状 + 材质 去重。
 */
#include "colliders.h"
#include "../world/blocks.h"
#include "../core/log.h"
#include <stdio.h>

typedef struct {
    float mass, friction, restitution, volume;
    bool has_mass;
} block_props;

typedef struct {
    float friction, restitution, volume;
    uint32_t collider;
} baked;

typedef struct {
    int nboxes;
    float *boxes;        /* 每箱 6 个数 */
    MCC_VEC(baked) baked;  /* 已按不同材质烘焙的碰撞体 */
    bool full;
} shape_def;

float *phys_state_mass;

static char *tok(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '\n' || *s == '\r') { *p = s; return NULL; }
    char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

static uint32_t shape_collider(shape_def *sh, const block_props *bp) {
    if (!sh->nboxes) return 0;
    for (size_t i = 0; i < sh->baked.len; i++) {
        const baked *k = &sh->baked.data[i];
        if (k->friction == bp->friction && k->restitution == bp->restitution && k->volume == bp->volume) return k->collider;
    }
    uint32_t id = phys_collider_new(bp->friction, bp->volume, bp->restitution, false, false, 0);
    for (int b = 0; b < sh->nboxes; b++) phys_collider_add_box(id, sh->boxes + b * 6, sh->boxes + b * 6 + 3);
    mcc_vec_push(&sh->baked, ((baked){ bp->friction, bp->restitution, bp->volume, id }));
    return id;
}

static bool name_matches(const char *pat, const char *name) {
    if (pat[0] != '*') return !strcmp(pat, name);
    size_t lp = strlen(pat + 1), ln = strlen(name);
    return ln >= lp && !strcmp(name + ln - lp, pat + 1);
}

/* 读属性表，填到每种方块上（后写的覆盖先写的） */
static bool load_props(const char *path, block_props *props) {
    for (uint32_t b = 0; b < mcc_blocks.block_count; b++) props[b] = (block_props){ 1.0f, 1.0f, 0.0f, 1.0f, false };
    if (!path) return true;
    FILE *f = fopen(path, "r");
    if (!f) { mcc_error("无法打开方块物理属性文件 %s", path); return false; }
    char *line = NULL;
    size_t cap = 0;
    int lineno = 0, entries = 0;
    bool ok = true;
    while (mcc_getline(&line, &cap, f) > 0) {
        lineno++;
        char *p = line, *t = tok(&p);
        if (!t || t[0] == '#') continue;
        char *pat = tok(&p);
        if (strcmp(t, "P") || !pat) { ok = false; break; }
        char full[256];
        if (pat[0] != '*' && !strchr(pat, ':')) { snprintf(full, sizeof full, "minecraft:%s", pat); pat = full; }
        block_props set = { 0 };
        bool hm = false, hf = false, hr = false, hv = false;
        for (char *kv; (kv = tok(&p));) {
            char *eq = strchr(kv, '=');
            if (!eq) { ok = false; break; }
            *eq = 0;
            float v = strtof(eq + 1, NULL);
            if (!strcmp(kv, "mass")) { set.mass = v; hm = true; }
            else if (!strcmp(kv, "friction")) { set.friction = v; hf = true; }
            else if (!strcmp(kv, "restitution")) { set.restitution = v; hr = true; }
            else if (!strcmp(kv, "volume")) { set.volume = v; hv = true; }
            else { ok = false; break; }
        }
        if (!ok) break;
        int matched = 0;
        for (uint32_t b = 0; b < mcc_blocks.block_count; b++) {
            if (!name_matches(pat, mcc_blocks.blocks[b].name)) continue;
            matched++;
            if (hm) { props[b].mass = set.mass; props[b].has_mass = true; }
            if (hf) props[b].friction = set.friction;
            if (hr) props[b].restitution = set.restitution;
            if (hv) props[b].volume = set.volume;
        }
        if (!matched) mcc_warn("方块物理属性第 %d 行：%s 没有匹配的方块", lineno, pat);
        entries++;
    }
    free(line);
    fclose(f);
    if (!ok) { mcc_error("方块物理属性文件 %s 第 %d 行格式错误", path, lineno); return false; }
    mcc_info("方块物理属性载入：%d 条", entries);
    return true;
}

bool phys_block_colliders_load(const char *path, const char *props_path) {
    if (!mcc_blocks.states) mcc_log_fatal("载入碰撞形状前须先载入方块注册表");
    block_props *props = mcc_xmalloc(sizeof *props * mcc_blocks.block_count);
    if (!load_props(props_path, props)) { free(props); return false; }
    free(phys_state_mass);
    phys_state_mass = mcc_xcalloc(mcc_blocks.state_count, sizeof(float));
    FILE *f = fopen(path, "r");
    if (!f) { mcc_error("无法打开碰撞形状文件 %s", path); free(props); return false; }
    MCC_VEC(shape_def) shapes = {0};
    size_t len = 0;
    char *line = NULL;
    int lineno = 0;
    uint32_t blocks = 0;
    bool ok = true;
    while (mcc_getline(&line, &len, f) > 0) {
        lineno++;
        char *p = line, *t = tok(&p);
        if (!t || t[0] == '#') continue;
        if (!strcmp(t, "S")) {
            char *a = tok(&p), *b = tok(&p);
            if (!a || !b) { ok = false; break; }
            size_t id = strtoul(a, NULL, 10);
            while (shapes.len <= id) mcc_vec_push(&shapes, (shape_def){0});
            shape_def *sh = &shapes.data[id];
            sh->nboxes = atoi(b);
            sh->boxes = mcc_xmalloc(sizeof(float) * 6 * (size_t)(sh->nboxes ? sh->nboxes : 1));
            for (int i = 0; i < sh->nboxes * 6; i++) {
                char *v = tok(&p);
                if (!v) { ok = false; break; }
                sh->boxes[i] = strtof(v, NULL);
            }
            const float *bx = sh->boxes;
            sh->full = sh->nboxes == 1 && bx[0] == 0 && bx[1] == 0 && bx[2] == 0 && bx[3] == 1 && bx[4] == 1 && bx[5] == 1;
        } else if (!strcmp(t, "C")) {
            char *name = tok(&p), *cnt = tok(&p);
            if (!name || !cnt) { ok = false; break; }
            int32_t b = mcc_block_by_name(name);
            if (b < 0) { mcc_warn("碰撞形状：未知方块 %s", name); continue; }
            const mcc_block_def *d = &mcc_blocks.blocks[b];
            uint32_t n = (uint32_t)strtoul(cnt, NULL, 10);
            if (n != 1 && n != d->state_count) { ok = false; break; }
            uint32_t sid = 0;
            for (uint32_t s = 0; s < d->state_count; s++) {
                if (s < n) {
                    char *v = tok(&p);
                    if (!v) { ok = false; break; }
                    sid = (uint32_t)strtoul(v, NULL, 10);
                }
                if (sid >= shapes.len) { ok = false; break; }
                shape_def *sh = &shapes.data[sid];
                mcc_state_info *si = &mcc_blocks.states[d->first_state + s];
                uint32_t col = shape_collider(sh, &props[b]);
                if (col > UINT16_MAX) mcc_log_fatal("碰撞体数量超过 65535");
                si->collider = (uint16_t)col;
                si->pflags = (uint8_t)((col ? MCC_PHYS_SOLID : 0) | (sh->full ? MCC_PHYS_FULL : 0) |
                                       (si->fluid ? MCC_PHYS_LIQUID : 0));
                /* 质量：属性表指定的优先；否则有碰撞箱 1.0，无碰撞箱的非空气方块 0.1（纯流体、空气为 0） */
                float m = props[b].has_mass ? props[b].mass : col ? 1.0f : 0.1f;
                bool pure_fluid = !col && si->fluid && (!strcmp(d->name, "minecraft:water") || !strcmp(d->name, "minecraft:lava") ||
                                                        !strcmp(d->name, "minecraft:bubble_column"));
                if (mcc_state_is_air(d->first_state + s) || pure_fluid) m = 0;
                phys_state_mass[d->first_state + s] = m;
            }
            if (!ok) break;
            blocks++;
        } else {
            ok = false;
            break;
        }
    }
    free(line);
    fclose(f);
    for (size_t i = 0; i < shapes.len; i++) {
        free(shapes.data[i].boxes);
        mcc_vec_free(&shapes.data[i].baked);
    }
    free(props);
    mcc_vec_free(&shapes);
    if (!ok) { mcc_error("碰撞形状文件 %s 第 %d 行格式错误", path, lineno); return false; }
    /* 没有碰撞形状记录的状态也要标上流体 */
    for (uint32_t s = 0; s < mcc_blocks.state_count; s++)
        if (mcc_blocks.states[s].fluid) mcc_blocks.states[s].pflags |= MCC_PHYS_LIQUID;
    mcc_info("方块碰撞体载入：%u 种方块，%u 个碰撞体", blocks, phys_collider_count());
    return true;
}
