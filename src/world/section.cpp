#include "section.h"
#include "../core/log.h"

mcc_chunk_section *mcc_section_new(mcc_state_t fill) {
    mcc_chunk_section *s = (mcc_chunk_section*)mcc_xcalloc(1, sizeof *s);
    s->single = fill;
    s->non_air = mcc_state_is_air(fill) ? 0 : MCC_SECTION_BLOCKS;
    s->nonzero = fill ? MCC_SECTION_BLOCKS : 0;
    return s;
}

void mcc_section_free(mcc_chunk_section *s) {
    if (!s) return;
    free(s->palette);
    free(s->data);
    free(s->phys);
    free(s);
}

MCC_INLINE void put4(uint8_t *d, int idx, uint32_t v) {
    int sh = (idx & 1) << 2;
    d[idx >> 1] = (uint8_t)((d[idx >> 1] & ~(15u << sh)) | (v << sh));
}

/* 单值 → 4 位 */
static void grow_from_single(mcc_chunk_section *s) {
    s->palette = (uint16_t*)mcc_xmalloc(16 * sizeof(uint16_t));
    s->palette[0] = (uint16_t)s->single;
    s->palette_len = 1;
    s->d8 = (uint8_t*)mcc_xcalloc(MCC_SECTION_BLOCKS / 2, 1);
    s->bits = 4;
}

/* 4 位 → 8 位 */
static void grow_4_to_8(mcc_chunk_section *s) {
    uint8_t *nd = (uint8_t*)mcc_xmalloc(MCC_SECTION_BLOCKS);
    for (int i = 0; i < MCC_SECTION_BLOCKS; i++) nd[i] = (s->d8[i >> 1] >> ((i & 1) << 2)) & 15;
    free(s->d8);
    s->d8 = nd;
    s->palette = (uint16_t*)mcc_xrealloc(s->palette, 256 * sizeof(uint16_t));
    s->bits = 8;
}

/* 8 位 → 直接 16 位 */
static void grow_8_to_16(mcc_chunk_section *s) {
    uint16_t *nd = (uint16_t*)mcc_xmalloc(MCC_SECTION_BLOCKS * sizeof(uint16_t));
    for (int i = 0; i < MCC_SECTION_BLOCKS; i++) nd[i] = s->palette[s->d8[i]];
    free(s->d8);
    free(s->palette);
    s->palette = NULL;
    s->palette_len = 0;
    s->d16 = nd;
    s->bits = 16;
}

/* 在调色板中查找或追加，必要时扩宽；返回调色板下标（bits 已可能变为 16，此时返回 -1） */
static int palette_index(mcc_chunk_section *s, mcc_state_t state) {
    for (int i = 0; i < s->palette_len; i++)
        if (s->palette[i] == state) return i;
    int cap = s->bits == 4 ? 16 : 256;
    if (s->palette_len == cap) {
        if (s->bits == 4) grow_4_to_8(s);
        else { grow_8_to_16(s); return -1; }
    }
    s->palette[s->palette_len] = (uint16_t)state;
    return s->palette_len++;
}

mcc_state_t mcc_section_set(mcc_chunk_section *s, int idx, mcc_state_t state) {
    mcc_state_t old = mcc_section_get(s, idx);
    if (old == state) return old;
    s->non_air += (uint16_t)(mcc_state_is_air(old) - mcc_state_is_air(state));
    s->nonzero += (uint16_t)((old == 0) - (state == 0));
    if (s->bits == 0) grow_from_single(s);
    if (s->bits != 16) {
        int pi = palette_index(s, state);
        if (pi >= 0) {
            if (s->bits == 4) put4(s->d8, idx, (uint32_t)pi);
            else s->d8[idx] = (uint8_t)pi;
            return old;
        }
    }
    s->d16[idx] = (uint16_t)state;
    return old;
}

void mcc_section_flag_rows(const mcc_chunk_section *s, uint16_t solid[16][16], uint16_t liquid[16][16], uint16_t full[16][16]) {
    if (s->bits == 0) {
        uint8_t f = mcc_state_pflags(s->single);
        for (int y = 0; y < 16; y++)
            for (int z = 0; z < 16; z++) {
                if (solid) solid[y][z] = (f & MCC_PHYS_SOLID) ? 0xFFFF : 0;
                if (liquid) liquid[y][z] = (f & MCC_PHYS_LIQUID) ? 0xFFFF : 0;
                if (full) full[y][z] = (f & MCC_PHYS_FULL) ? 0xFFFF : 0;
            }
        return;
    }
    uint8_t pf[256];
    for (int i = 0; i < s->palette_len; i++) pf[i] = mcc_state_pflags(s->palette[i]);
    int idx = 0;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) {
            uint32_t rs = 0, rl = 0, rf = 0;
            for (int x = 0; x < 16; x++, idx++) {
                uint8_t f;
                if (s->bits == 4) f = pf[(s->d8[idx >> 1] >> ((idx & 1) << 2)) & 15];
                else if (s->bits == 8) f = pf[s->d8[idx]];
                else f = mcc_state_pflags(s->d16[idx]);
                rs |= (uint32_t)(f & MCC_PHYS_SOLID) << x;
                rf |= (uint32_t)((f & MCC_PHYS_FULL) >> 1) << x;
                rl |= (uint32_t)((f & MCC_PHYS_LIQUID) >> 2) << x;
            }
            if (solid) solid[y][z] = (uint16_t)rs;
            if (liquid) liquid[y][z] = (uint16_t)rl;
            if (full) full[y][z] = (uint16_t)rf;
        }
}

void mcc_section_set_phys(mcc_chunk_section *s, int x, int y, int z, bool solid, bool liquid) {
    if (!s->phys) {
        if (!solid && !liquid) return;
        s->phys = (mcc_section_phys*)mcc_xcalloc(1, sizeof *s->phys);
    }
    uint16_t m = (uint16_t)(1u << x);
    uint16_t *rs = &s->phys->solid[y][z], *rl = &s->phys->liquid[y][z];
    if (solid != (bool)(*rs & m)) {
        *rs ^= m;
        s->solid_count += solid ? 1 : (uint16_t)-1;
    }
    if (liquid != (bool)(*rl & m)) {
        *rl ^= m;
        s->liquid_count += liquid ? 1 : (uint16_t)-1;
    }
    if (!s->solid_count && !s->liquid_count) {
        free(s->phys);
        s->phys = NULL;
    }
}

size_t mcc_section_memory(const mcc_chunk_section *s) {
    size_t n = sizeof *s;
    if (s->bits == 4) n += 16 * sizeof(uint16_t) + MCC_SECTION_BLOCKS / 2;
    else if (s->bits == 8) n += 256 * sizeof(uint16_t) + MCC_SECTION_BLOCKS;
    else if (s->bits == 16) n += MCC_SECTION_BLOCKS * sizeof(uint16_t);
    if (s->phys) n += sizeof *s->phys;
    return n;
}
