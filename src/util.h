/* Small helpers shared by every nvmetal module. */
#ifndef NVMETAL_UTIL_H
#define NVMETAL_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NVM_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NVM_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NVM_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define NVM_IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

#define NVM_PAGE_SHIFT 12
#define NVM_PAGE_SIZE  (1u << NVM_PAGE_SHIFT)
#define NVM_HUGE_SHIFT 21
#define NVM_HUGE_SIZE  (1u << NVM_HUGE_SHIFT)

enum nvm_log_level { NVM_LOG_ERR = 0, NVM_LOG_WARN = 1, NVM_LOG_INFO = 2, NVM_LOG_DEBUG = 3 };

/* Threshold read once from NVMETAL_LOG (err|warn|info|debug); default warn. */
int nvm_log_level(void);
void nvm_log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define nvm_log(level, ...)                                                                        \
    do {                                                                                           \
        if ((level) <= nvm_log_level())                                                            \
            nvm_log_msg((level), __VA_ARGS__);                                                     \
    } while (0)
#define nvm_err(...)   nvm_log(NVM_LOG_ERR, __VA_ARGS__)
#define nvm_warn(...)  nvm_log(NVM_LOG_WARN, __VA_ARGS__)
#define nvm_info(...)  nvm_log(NVM_LOG_INFO, __VA_ARGS__)
#define nvm_debug(...) nvm_log(NVM_LOG_DEBUG, __VA_ARGS__)

static inline uint64_t nvm_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
