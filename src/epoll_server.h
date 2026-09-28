/*
 * epoll_server.h — Linux epoll-Based Event Loop Server Interface
 *
 * OS Concept (Week 10):
 *   epoll(7) is a Linux-specific I/O event notification mechanism.
 *   Unlike select()/poll() which scan all fds on every call, epoll maintains
 *   a kernel-side list of watched fds. epoll_wait() returns ONLY the fds
 *   that are ready, making it O(active events) rather than O(total fds).
 *
 *   This enables a single thread to manage thousands of simultaneous
 *   connections without blocking on any single one.
 *
 * NOTE: This file is Linux-specific.
 *       On macOS, compilation proceeds but the server mode will print a
 *       warning and exit. Run it on Ubuntu.
 */

#ifndef MICROHTTP_EPOLL_SERVER_H
#define MICROHTTP_EPOLL_SERVER_H

/* ─── Configuration ───────────────────────────────────────────────────────── */
typedef struct {
    int         listen_fd;      /* already-bound listening socket              */
    const char *doc_root;       /* document root for file serving              */
    int         idle_timeout;   /* seconds before idle connections are closed  */
    int         max_connections;/* maximum simultaneous open connections        */
} epoll_server_config_t;

/*
 * epoll_server_run — start the epoll event loop (blocks until shutdown).
 *
 * Returns 0 on clean shutdown, -1 on fatal error.
 * This is Linux-only. On other platforms it prints a message and returns -1.
 */
int epoll_server_run(const epoll_server_config_t *cfg);

#endif /* MICROHTTP_EPOLL_SERVER_H */
