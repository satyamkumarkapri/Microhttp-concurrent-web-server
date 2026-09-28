/*
 * signal_handler.h — Graceful Shutdown Signal Handling Interface
 *
 * OS Concept (Week 6):
 *   POSIX signals are asynchronous notifications delivered to a process.
 *   Writing complex logic inside a signal handler is unsafe because:
 *   1. Signal handlers interrupt the main thread at arbitrary points.
 *   2. Most library functions (including malloc, printf) are NOT async-signal-safe.
 *   3. Re-entrant code issues can cause deadlocks.
 *
 *   Linux-safe solution: use signalfd(2) to receive signals as readable
 *   file descriptor events. The signal is "converted" into an fd that
 *   we can poll with epoll or select, allowing safe handling in the
 *   normal event loop without async-signal dangers.
 *
 *   Alternative (used in single/threadpool mode): write one byte to a
 *   self-pipe from the signal handler (write(2) is async-signal-safe),
 *   then check the pipe in the main loop.
 *
 * This module uses signalfd on Linux and a self-pipe on other POSIX systems.
 */

#ifndef MICROHTTP_SIGNAL_HANDLER_H
#define MICROHTTP_SIGNAL_HANDLER_H

#include <stdatomic.h>

/*
 * Global shutdown flag — set to 1 when shutdown is requested.
 * Declared volatile + atomic so all threads see the update immediately.
 * Use atomic_load() to read, atomic_store() to write.
 */
extern volatile atomic_int g_shutdown_requested;

/*
 * signal_handler_init — set up signal handling for SIGINT and SIGTERM.
 *
 * Returns a file descriptor that becomes readable when a shutdown signal
 * is received (signalfd on Linux, read end of a self-pipe elsewhere).
 * Returns -1 on error.
 *
 * The caller must close this fd when done.
 */
int signal_handler_init(void);

/*
 * signal_handler_cleanup — unblock signals and close the signal fd.
 */
void signal_handler_cleanup(int signal_fd);

/*
 * signal_handler_check — non-blocking check; consume any pending signal
 * from signal_fd and set g_shutdown_requested.
 *
 * Returns 1 if shutdown was signalled, 0 otherwise.
 */
int signal_handler_check(int signal_fd);

#endif /* MICROHTTP_SIGNAL_HANDLER_H */
