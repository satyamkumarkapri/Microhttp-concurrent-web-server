/*
 * server.h — TCP Listener and Server Modes Interface
 *
 * OS Concept (Week 1):
 *   The TCP server lifecycle:
 *   1. socket()   — create an endpoint in the kernel
 *   2. setsockopt() — set SO_REUSEADDR (avoid "Address already in use")
 *   3. bind()     — assign the local address/port
 *   4. listen()   — create the accept queue (backlog)
 *   5. accept()   — dequeue one completed TCP connection
 *   6. (handle connection)
 *   7. close()    — release the file descriptor
 *
 * This module owns the listening socket. The three server modes (single,
 * threadpool, epoll) each drive the accept loop differently.
 */

#ifndef MICROHTTP_SERVER_H
#define MICROHTTP_SERVER_H

#include <stddef.h>

/* ─── Server mode ─────────────────────────────────────────────────────────── */
typedef enum {
    SERVER_MODE_SINGLE,       /* single-threaded, handles one connection at a time */
    SERVER_MODE_THREADPOOL,   /* thread pool: workers handle connections in parallel */
    SERVER_MODE_EPOLL         /* Linux epoll event loop (single-threaded, non-blocking) */
} server_mode_t;

/* ─── Server configuration ────────────────────────────────────────────────── */
typedef struct {
    server_mode_t mode;
    int           port;
    const char   *doc_root;
    int           num_workers;      /* thread pool workers (threadpool mode)  */
    int           queue_size;       /* thread pool queue depth                */
    int           idle_timeout;     /* idle connection timeout (seconds)      */
    int           max_connections;  /* epoll mode connection limit            */
    int           quiet;            /* suppress info logs                     */
} server_config_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/*
 * server_create_listener — create, bind, and listen on a TCP socket.
 *
 * Returns the listening fd, or -1 on error.
 */
int server_create_listener(int port);

/*
 * server_run — enter the server's main loop.
 *
 * Blocks until g_shutdown_requested is set.
 * Returns 0 on clean shutdown.
 */
int server_run(int listen_fd, const server_config_t *cfg);

/* Default configuration values */
#define DEFAULT_PORT            8080
#define DEFAULT_WORKERS         4
#define DEFAULT_QUEUE_SIZE      128
#define DEFAULT_IDLE_TIMEOUT    10
#define DEFAULT_MAX_CONNECTIONS 1024
#define DEFAULT_DOC_ROOT        "./public"
#define LISTEN_BACKLOG          128

#endif /* MICROHTTP_SERVER_H */
