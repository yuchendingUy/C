#include "blocks.h"
#include "../core/log.h"
#include <stdio.h>
#include <string.h>

mcc_block_registry mcc_blocks;

/* 名称索引：开放寻址字符串哈希 */
typedef struct { size_t mask; int32_t *slots; } name_index;

static uint64_t str_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static void index_build(void) {
    name_index *ix = mcc_xmalloc(sizeof *ix);
    size_t n = 16;
    while (n < mcc_blocks.block_count * 2) n <<= 1;
    ix->mask = n - 1;
    ix->slots = mcc_xmalloc(n * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) ix->slots[i] = -1;
    for (uint32_t b = 0; b < mcc_blocks.block_count; b++) {
        size_t i = str_hash(mcc_blocks.blocks[b].name) & ix->mask;
        while (ix->slots[i] >= 0) i = (i + 1) & ix->mask;
        ix->slots[i] = (int32_t)b;
    }
    mcc_blocks.name_index = ix;
}

int32_t mcc_block_by_name(const char *name) {
    char buf[256];
    if (!strchr(name, ':')) { snprintf(buf, sizeof buf, "minecraft:%s", name); name = buf; }
    name_index *ix = mcc_blocks.name_index;
    size_t i = str_hash(name) & ix->mask;
    while (ix->slots[i] >= 0) {
        if (!strcmp(mcc_blocks.blocks[ix->slots[i]].name, name)) return ix->slots[i];
        i = (i + 1) & ix->mask;
    }
    return -1;
}

mcc_state_t mcc_block_default_state(const char *name) {
    int32_t b = mcc_block_by_name(name);
    if (b < 0) mcc_log_fatal("方块 %s 不存在", name);
    return mcc_blocks.blocks[b].default_state;
}

static char *next_tok(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '\n') { *p = s; return NULL; }
    char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

bool mcc_blocks_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { mcc_error("无法打开方块注册表 %s", path); return false; }
    MCC_VEC(mcc_block_def) blocks = {0};
    char line[8192];
    mcc_block_def *cur = NULL;
    int prop_fill = 0;
    uint32_t expect_state = 0;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        char *tag = next_tok(&p);
        if (!tag || tag[0] == '#') continue;
        if (!strcmp(tag, "B")) {
            if (cur && prop_fill != cur->nprops) goto bad;
            mcc_block_def d = {0};
            char *t[10];
            for (int i = 0; i < 10; i++) if (!(t[i] = next_tok(&p))) goto bad;
            d.name = mcc_xstrdup(t[0]);
            d.first_state = (mcc_state_t)strtoul(t[1], NULL, 10);
            d.state_count = (uint32_t)strtoul(t[2], NULL, 10);
            d.default_state = (mcc_state_t)strtoul(t[3], NULL, 10);
            d.hardness = strtof(t[4], NULL);
            d.resistance = strtof(t[5], NULL);
            d.flags = (uint8_t)atoi(t[6]);
            d.emit_light = (uint8_t)atoi(t[7]);
            d.filter_light = (uint8_t)atoi(t[8]);
            d.nprops = (uint8_t)atoi(t[9]);
            d.props = d.nprops ? mcc_xcalloc(d.nprops, sizeof(mcc_block_prop)) : NULL;
            if (d.first_state != expect_state || !d.state_count) goto bad;
            expect_state += d.state_count;
            mcc_vec_push(&blocks, d);
            cur = &blocks.data[blocks.len - 1];
            prop_fill = 0;
        } else if (!strcmp(tag, "P")) {
            if (!cur || prop_fill >= cur->nprops) goto bad;
            mcc_block_prop *pr = &cur->props[prop_fill++];
            char *n = next_tok(&p), *c = next_tok(&p);
            if (!n || !c) goto bad;
            pr->name = mcc_xstrdup(n);
            pr->nvalues = atoi(c);
            pr->values = mcc_xcalloc((size_t)pr->nvalues, sizeof(char *));
            for (int i = 0; i < pr->nvalues; i++) {
                char *v = next_tok(&p);
                if (!v) goto bad;
                pr->values[i] = mcc_xstrdup(v);
            }
        } else {
            goto bad;
        }
    }
    fclose(f);
    if (cur && prop_fill != cur->nprops) { lineno++; goto bad_closed; }

    mcc_blocks.blocks = blocks.data;
    mcc_blocks.block_count = (uint32_t)blocks.len;
    mcc_blocks.state_count = expect_state;
    mcc_blocks.states = mcc_xcalloc(expect_state, sizeof(mcc_state_info));
    for (uint32_t b = 0; b < mcc_blocks.block_count; b++) {
        mcc_block_def *d = &mcc_blocks.blocks[b];
        /* 步长：最后一个属性变化最快 */
        uint32_t stride = 1, prod = 1;
        for (int i = d->nprops - 1; i >= 0; i--) {
            d->props[i].stride = stride;
            stride *= (uint32_t)d->props[i].nvalues;
        }
        for (int i = 0; i < d->nprops; i++) prod *= (uint32_t)d->props[i].nvalues;
        if (prod != d->state_count) mcc_log_fatal("方块 %s 属性组合数 %u 与状态数 %u 不符", d->name, prod, d->state_count);
        bool air = !strcmp(d->name, "minecraft:air") || !strcmp(d->name, "minecraft:cave_air") ||
                   !strcmp(d->name, "minecraft:void_air");
        for (uint32_t s = 0; s < d->state_count; s++) {
            mcc_state_info *si = &mcc_blocks.states[d->first_state + s];
            si->block = (mcc_block_t)b;
            si->flags = (uint8_t)(d->flags | (air ? 0x80 : 0));
            si->light = (uint8_t)((d->emit_light & 15) | ((d->filter_light & 15) << 4));
        }
    }
    int bits = 0;
    while ((1u << bits) < mcc_blocks.state_count) bits++;
    mcc_blocks.direct_bits = bits;
    index_build();
    mcc_info("方块注册表载入：%u 种方块，%u 个状态，直接模式 %d 位", mcc_blocks.block_count, mcc_blocks.state_count, bits);
    return true;

bad:
    fclose(f);
bad_closed:
    mcc_error("方块注册表 %s 第 %d 行格式错误", path, lineno);
    return false;
}

void mcc_blocks_free(void) {
    for (uint32_t b = 0; b < mcc_blocks.block_count; b++) {
        mcc_block_def *d = &mcc_blocks.blocks[b];
        for (int i = 0; i < d->nprops; i++) {
            for (int v = 0; v < d->props[i].nvalues; v++) free(d->props[i].values[v]);
            free(d->props[i].values);
            free(d->props[i].name);
        }
        free(d->props);
        free(d->name);
    }
    free(mcc_blocks.blocks);
    free(mcc_blocks.states);
    if (mcc_blocks.name_index) {
        free(((name_index *)mcc_blocks.name_index)->slots);
        free(mcc_blocks.name_index);
    }
    memset(&mcc_blocks, 0, sizeof mcc_blocks);
}

int mcc_state_prop_index(mcc_state_t s, const char *prop) {
    const mcc_block_def *d = mcc_state_def(s);
    for (int i = 0; i < d->nprops; i++)
        if (!strcmp(d->props[i].name, prop)) return i;
    return -1;
}

int mcc_state_get(mcc_state_t s, int prop) {
    const mcc_block_def *d = mcc_state_def(s);
    const mcc_block_prop *p = &d->props[prop];
    return (int)(((s - d->first_state) / p->stride) % (uint32_t)p->nvalues);
}

mcc_state_t mcc_state_with(mcc_state_t s, int prop, int value) {
    const mcc_block_def *d = mcc_state_def(s);
    const mcc_block_prop *p = &d->props[prop];
    int cur = (int)(((s - d->first_state) / p->stride) % (uint32_t)p->nvalues);
    return (mcc_state_t)((int64_t)s + (int64_t)(value - cur) * p->stride);
}

const char *mcc_state_get_str(mcc_state_t s, const char *prop) {
    int i = mcc_state_prop_index(s, prop);
    if (i < 0) return NULL;
    return mcc_state_def(s)->props[i].values[mcc_state_get(s, i)];
}

mcc_state_t mcc_state_with_str(mcc_state_t s, const char *prop, const char *value) {
    int i = mcc_state_prop_index(s, prop);
    if (i < 0) return s;
    const mcc_block_prop *p = &mcc_state_def(s)->props[i];
    for (int v = 0; v < p->nvalues; v++)
        if (!strcmp(p->values[v], value)) return mcc_state_with(s, i, v);
    return s;
}

void mcc_state_describe(mcc_state_t s, char *buf, size_t len) {
    const mcc_block_def *d = mcc_state_def(s);
    size_t n = (size_t)snprintf(buf, len, "%s", d->name);
    for (int i = 0; i < d->nprops && n < len; i++)
        n += (size_t)snprintf(buf + n, len - n, "%s%s=%s", i ? "," : "[", d->props[i].name,
                              d->props[i].values[mcc_state_get(s, i)]);
    if (d->nprops && n < len) snprintf(buf + n, len - n, "]");
}
