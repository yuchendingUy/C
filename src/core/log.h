#ifndef MCC_LOG_H
#define MCC_LOG_H

enum mcc_log_level { MCC_LOG_DEBUG, MCC_LOG_INFO, MCC_LOG_WARN, MCC_LOG_ERROR, MCC_LOG_FATAL };

void mcc_log_set_level(enum mcc_log_level lvl);
void mcc_log(enum mcc_log_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
_Noreturn void mcc_log_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define mcc_debug(...) mcc_log(MCC_LOG_DEBUG, __VA_ARGS__)
#define mcc_info(...)  mcc_log(MCC_LOG_INFO, __VA_ARGS__)
#define mcc_warn(...)  mcc_log(MCC_LOG_WARN, __VA_ARGS__)
#define mcc_error(...) mcc_log(MCC_LOG_ERROR, __VA_ARGS__)

#endif
