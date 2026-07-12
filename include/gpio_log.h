/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpio_log.h — tiny leveled logger shared across the daemon. */
#ifndef GPIO_LOG_H
#define GPIO_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GPIO_LOG_ERROR = 0,
    GPIO_LOG_WARN  = 1,
    GPIO_LOG_INFO  = 2,
    GPIO_LOG_DEBUG = 3,
};

/* Current threshold; messages at or below this level are printed. Default INFO. */
extern int gpio_log_level;

void gpio_log(int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Parse "error|warn|info|debug" (or a digit 0-3) into a level. Returns the level,
 * or GPIO_LOG_INFO if unrecognised. */
int gpio_log_level_from_str(const char *s);

#define LOG_ERR(...)  gpio_log(GPIO_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...) gpio_log(GPIO_LOG_WARN,  __VA_ARGS__)
#define LOG_INFO(...) gpio_log(GPIO_LOG_INFO,  __VA_ARGS__)
#define LOG_DBG(...)  gpio_log(GPIO_LOG_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* GPIO_LOG_H */
