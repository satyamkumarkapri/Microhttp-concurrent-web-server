/*
 * logger.h — MICROHTTP Logging Interface
 *
 * Provides thread-safe logging macros with severity levels.
 * All log output goes to stderr.
 *
 * Note: _GNU_SOURCE is defined in the Makefile (-D_GNU_SOURCE), not here.
 * Using __VA_ARGS__ without ## to stay compatible with -Wpedantic.
 *
 * OS Concept: Demonstrates safe concurrent output using flockfile/funlockfile
 * (POSIX stdio locking) to avoid interleaved output from multiple threads.
 */

#ifndef MICROHTTP_LOGGER_H
#define MICROHTTP_LOGGER_H

#include <stdio.h>
#include <time.h>

/* Log level constants — lower value = more verbose */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_NONE  = 4   /* suppress all output */
} log_level_t;

/* Global log level — set once before threads are spawned */
extern log_level_t g_log_level;

/* Internal helper: never call directly */
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Public macros ─────────────────────────────────────────────────────────── */
/*
 * We use __VA_OPT__(,) to handle the zero-argument case portably.
 * This avoids the non-standard GNU ## __VA_ARGS__ extension.
 * __VA_OPT__ requires C23 or GNU C11 extension. As a simpler portable
 * alternative, we always require at least one variadic argument (the format
 * string already counts). In practice every call site has a format string
 * so ##__VA_ARGS__ is fine on GCC/Clang; we suppress the pedantic warning.
 */
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#  pragma GCC diagnostic ignored "-Wvariadic-macros"
#endif

#define LOG_DEBUG(fmt, ...) \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* Convenience: log a system error using errno */
#define LOG_SYSERR(msg) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, "%s: %m", (msg))

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#endif /* MICROHTTP_LOGGER_H */
