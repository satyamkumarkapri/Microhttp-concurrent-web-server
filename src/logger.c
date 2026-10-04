/*
 * logger.c — MICROHTTP Logging Implementation
 *
 * Thread-safe logging using flockfile/funlockfile (POSIX).
 * Uses strerror(errno) for system error messages.
 *
 * OS Concept: stdio locking — POSIX guarantees that individual stdio
 * operations on the same FILE* are atomic (protected internally), but
 * we use flockfile to make the entire formatted line atomic across threads.
 */

/* _GNU_SOURCE is set via Makefile -D flag */

#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* Default log level: show INFO and above */
log_level_t g_log_level = LOG_LEVEL_INFO;

static const char *level_str(log_level_t level)
{
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO ";
        case LOG_LEVEL_WARN:  return "WARN ";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "?????";
    }
}

/*
 * log_write — format and emit one log line.
 *
 * We use flockfile(stderr)/funlockfile(stderr) to ensure that the entire
 * line is written without being interleaved by another thread's log call.
 * This is the POSIX-defined way to do atomic multi-call stdio output.
 */
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    if (level < g_log_level)
        return;

    /* Format the timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);  /* thread-safe localtime */
    char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    /* Lock stderr for the entire line to prevent interleaving */
    flockfile(stderr);

    fprintf(stderr, "[%s][%s] ", ts, level_str(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    /* Only show file:line in DEBUG mode to reduce noise */
    if (level == LOG_LEVEL_DEBUG) {
        /* Extract just the basename of __FILE__ */
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        fprintf(stderr, "  (%s:%d)", base, line);
    }

    fputc('\n', stderr);
    funlockfile(stderr);
}
