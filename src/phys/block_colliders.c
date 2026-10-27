/*
 * 方块状态 → 碰撞体（对应 Sable 按方块状态烘焙碰撞体）
 *
 * 碰撞形状来自原版数据（tools/gen_vanilla_data.py 生成的 shapes.txt）。
 * 物理属性（摩擦倍率、浮力体积、弹性）暂时统一用默认值，按方块定制的属性表后续接入。
 */
#include "colliders.h"
#include "../world/blocks.h"
#include "../core/log.h"
#include <stdio.h>

typedef struct {
    int nboxes;
    float *boxes;        /* 每箱 6 个数 */
    uint32_t collider;   /* 已烘焙的碰撞体编号，0 未烘焙 */
    bool full;
} shape_def;

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

static uint32_t shape_collider(shape_def *sh) {
    if (!sh->nboxes) return 0;
    if (!sh->collider) {
        sh->collider = phys_collider_new(1.0f, 1.0f, 0.0f, false, false, 0);
        for (int b = 0; b < sh->nboxes; b++) phys_collider_add_box(sh->collider, sh->boxes + b * 6, sh->boxes + b * 6 + 3);
    }
    return sh->collider;
}

bool phys_block_colliders_load(const char *path) {
    if (!mcc_blocks.states) mcc_log_fatal("载入碰撞形状前须先载入方块注册表");
    FILE *f = fopen(path, "r");
    if (!f) { mcc_error("无法打开碰撞形状文件 %s", path); return false; }
    MCC_VEC(shape_def) shapes = {0};
    size_t len = 0;
    char *line = NULL;
    int lineno = 0;
    uint32_t blocks = 0;
    bool ok = true;
    while (getline(&line, &len, f) > 0) {
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
                uint32_t col = shape_collider(sh);
                if (col > UINT16_MAX) mcc_log_fatal("碰撞体数量超过 65535");
                si->collider = (uint16_t)col;
                si->pflags = (uint8_t)((col ? MCC_PHYS_SOLID : 0) | (sh->full ? MCC_PHYS_FULL : 0) |
                                       (si->fluid ? MCC_PHYS_LIQUID : 0));
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
    for (size_t i = 0; i < shapes.len; i++) free(shapes.data[i].boxes);
    mcc_vec_free(&shapes);
    if (!ok) { mcc_error("碰撞形状文件 %s 第 %d 行格式错误", path, lineno); return false; }
    /* 没有碰撞形状记录的状态也要标上流体 */
    for (uint32_t s = 0; s < mcc_blocks.state_count; s++)
        if (mcc_blocks.states[s].fluid) mcc_blocks.states[s].pflags |= MCC_PHYS_LIQUID;
    mcc_info("方块碰撞体载入：%u 种方块，%u 个碰撞体", blocks, phys_collider_count());
    return true;
}
