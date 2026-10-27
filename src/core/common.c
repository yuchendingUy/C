#include "common.h"
#include "log.h"
#include <time.h>

static void oom(size_t n) {
    mcc_log_fatal("内存不足，申请 %zu 字节失败", n);
}

void *mcc_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (MCC_UNLIKELY(!p)) oom(n);
    return p;
}

void *mcc_xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (MCC_UNLIKELY(!p)) oom(n * sz);
    return p;
}

void *mcc_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (MCC_UNLIKELY(!q)) oom(n);
    return q;
}

char *mcc_xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = mcc_xmalloc(n);
    memcpy(d, s, n);
    return d;
}

void *mcc_xaligned(size_t align, size_t n) {
#ifdef _WIN32
    /* Windows 没有 aligned_alloc，且 _aligned_malloc 的内存不能用 free 释放。
     * 64 位 malloc 保证 16 字节对齐；要求 16 字节的地方（凸包）正确，要求缓存行对齐的只是性能 */
    (void)align;
    void *p = malloc(n ? n : 1);
#else
    size_t rounded = (n + align - 1) / align * align;
    void *p = aligned_alloc(align, rounded ? rounded : align);
#endif
    if (MCC_UNLIKELY(!p)) oom(n);
    return p;
}

long mcc_getline(char **line, size_t *cap, FILE *f) {
    size_t len = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 2 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *line = mcc_xrealloc(*line, *cap);
        }
        (*line)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (!len) return -1;
    (*line)[len] = 0;
    return (long)len;
}

uint64_t mcc_nanotime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
