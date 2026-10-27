#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

static enum mcc_log_level g_level = MCC_LOG_INFO;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const NAMES[] = { "调试", "信息", "警告", "错误", "致命" };

void mcc_log_set_level(enum mcc_log_level lvl) { g_level = lvl; }

static void vlog(enum mcc_log_level lvl, const char *fmt, va_list ap) {
    char tbuf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);
    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "[%s %s] ", tbuf, NAMES[lvl]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_lock);
}

void mcc_log(enum mcc_log_level lvl, const char *fmt, ...) {
    if (lvl < g_level) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(lvl, fmt, ap);
    va_end(ap);
}

void mcc_log_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(MCC_LOG_FATAL, fmt, ap);
    va_end(ap);
    abort();
}
