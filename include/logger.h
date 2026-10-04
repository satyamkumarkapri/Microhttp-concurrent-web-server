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
#include <string.h>
#include <errno.h>

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
 * Variadic macro handling for -std=c11 -Wpedantic:
 * We omit the named 'fmt' parameter and just pass __VA_ARGS__ to log_write.
 * This requires callers to provide at least a format string, satisfying C99's
 * requirement that variadic macros receive at least one argument.
 */

#define LOG_DEBUG(...) \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_INFO(...) \
    log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)

#define LOG_WARN(...) \
    log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)

#define LOG_ERROR(...) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

/* Convenience: log a system error using errno */
#define LOG_SYSERR(msg) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, "%s: %s", (msg), strerror(errno))

#endif /* MICROHTTP_LOGGER_H */
