#include "util.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int log_threshold = -1;

int nvm_log_level(void)
{
    /* A benign race: every thread computes the same value from the environment. */
    int lvl = __atomic_load_n(&log_threshold, __ATOMIC_RELAXED);
    if (lvl >= 0)
        return lvl;
    const char *e = getenv("NVMETAL_LOG");
    lvl = NVM_LOG_WARN;
    if (e) {
        if (!strcmp(e, "err"))
            lvl = NVM_LOG_ERR;
        else if (!strcmp(e, "info"))
            lvl = NVM_LOG_INFO;
        else if (!strcmp(e, "debug"))
            lvl = NVM_LOG_DEBUG;
    }
    __atomic_store_n(&log_threshold, lvl, __ATOMIC_RELAXED);
    return lvl;
}

void nvm_log_msg(int level, const char *fmt, ...)
{
    static const char *const names[] = {"error", "warn", "info", "debug"};
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* One fprintf so lines from different threads do not interleave. */
    fprintf(stderr, "nvmetal %s: %s\n", names[level & 3], buf);
}
