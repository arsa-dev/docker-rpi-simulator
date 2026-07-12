/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "gpio_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int gpio_log_level = GPIO_LOG_INFO;

static const char *level_tag(int level)
{
    switch (level) {
    case GPIO_LOG_ERROR: return "ERROR";
    case GPIO_LOG_WARN:  return "WARN ";
    case GPIO_LOG_INFO:  return "INFO ";
    case GPIO_LOG_DEBUG: return "DEBUG";
    default:             return "?????";
    }
}

void gpio_log(int level, const char *fmt, ...)
{
    if (level > gpio_log_level)
        return;

    /* Wall-clock HH:MM:SS prefix keeps demo/CI logs readable without pulling in a
     * logging library. */
    char ts[16] = "--:--:--";
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv))
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    FILE *out = (level <= GPIO_LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s] %s  ", ts, level_tag(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
    fflush(out);
}

int gpio_log_level_from_str(const char *s)
{
    if (!s || !*s)
        return GPIO_LOG_INFO;
    if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0')
        return s[0] - '0';
    if (strcmp(s, "error") == 0) return GPIO_LOG_ERROR;
    if (strcmp(s, "warn") == 0)  return GPIO_LOG_WARN;
    if (strcmp(s, "info") == 0)  return GPIO_LOG_INFO;
    if (strcmp(s, "debug") == 0) return GPIO_LOG_DEBUG;
    return GPIO_LOG_INFO;
}
