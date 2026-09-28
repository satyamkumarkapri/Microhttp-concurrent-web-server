/*
 * server.c — TCP Listener and Server Mode Dispatcher
 *
 * OS Concepts (Week 1, 4, 6):
 *
 * socket(AF_INET, SOCK_STREAM, 0):
 *   Requests the kernel to create a TCP socket (SOCK_STREAM = reliable,
 *   ordered, connection-oriented). Returns a file descriptor — an index
 *   into the process's open-file table.
 *
 * SO_REUSEADDR:
 *   Without this, restarting the server after Ctrl+C within ~2 minutes
 *   fails with "Address already in use" because the kernel keeps the port
 *   in TIME_WAIT. SO_REUSEADDR allows immediate reuse.
 *
 * bind(fd, addr, addrlen):
 *   Associates the socket with a specific IP address and port number.
 *   Writes the binding into the kernel's socket table.
 *
 * listen(fd, backlog):
 *   Transitions the socket from CLOSED to LISTEN state. The backlog
 *   parameter sets the size of the kernel's queue of fully-established
 *   but not-yet-accepted connections (the "accept queue").
 *
 * accept(fd, addr, addrlen):
 *   Dequeues one connection from the accept queue. Blocks if the queue
 *   is empty (in blocking mode). Returns a NEW file descriptor representing
 *   the established connection. The listening fd remains open for more accepts.
 *
 * Single-threaded mode:
 *   The simplest model. Accept → handle → repeat. While one connection is
 *   being served, all other clients wait. Useful as a baseline for benchmarking.
 */

#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "connection.h"
#include "http.h"
#include "parser.h"
#include "logger.h"
#include "signal_handler.h"
#include "thread_pool.h"
#include "epoll_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ─── TCP Listener Creation ───────────────────────────────────────────────── */

int server_create_listener(int port)
{
    /*
     * AF_INET = IPv4 address family
     * SOCK_STREAM = TCP (reliable, ordered, connection-oriented)
     * 0 = let kernel pick the protocol (TCP for SOCK_STREAM)
     * O_CLOEXEC set separately below for portability
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_SYSERR("socket");
        return -1;
    }

    /* Set close-on-exec flag (portable alternative to SOCK_CLOEXEC) */
#ifdef FD_CLOEXEC
    {
        int flags = fcntl(fd, F_GETFD, 0);
        if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif

    /*
     * SO_REUSEADDR: Allow binding to a port in TIME_WAIT.
     * val=1 enables the option. This is a kernel socket option, not a
     * user-space setting — it changes kernel behavior for this socket.
     */
    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
        LOG_SYSERR("setsockopt SO_REUSEADDR");
        close(fd);
        return -1;
    }

    /* Build the bind address: INADDR_ANY = accept on all interfaces */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);   /* host-to-network byte order */
    addr.sin_addr.s_addr = INADDR_ANY;

    /*
     * bind() — associate the socket with the address.
     * The kernel records the binding in the TCP demultiplexing table.
     * Incoming TCP segments with dst_port=port will be routed here.
     */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_SYSERR("bind");
        close(fd);
        return -1;
    }

    /*
     * listen() — transition to LISTEN state.
     * LISTEN_BACKLOG sets the accept queue depth.
     * If more than backlog connections complete the 3-way handshake before
     * accept() is called, the kernel silently drops the excess.
     */
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        LOG_SYSERR("listen");
        close(fd);
        return -1;
    }

    LOG_INFO("Listening on 0.0.0.0:%d (fd=%d)", port, fd);
    return fd;
}

/* ─── Single-threaded mode ────────────────────────────────────────────────── */

/*
 * handle_connection_single — process all HTTP requests on one connection.
 *
 * This is the simplest possible implementation: read → parse → respond,
 * loop for keep-alive, then return.
 *
 * No threads. No epoll. One client at a time.
 */
static void handle_connection_single(int client_fd, const char *doc_root,
                                     int idle_timeout)
{
    /* Set socket-level receive timeout for idle detection */
    struct timeval tv;
    tv.tv_sec  = idle_timeout;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    http_parser_t parser;
    http_parser_init(&parser);

    char buf[4096];
    int  keep_alive = 1;

    while (keep_alive && !atomic_load(&g_shutdown_requested)) {
        http_parser_init(&parser);

        parse_result_t result = PARSE_INCOMPLETE;
        http_request_t req;

        while (result == PARSE_INCOMPLETE) {
            ssize_t nr = recv(client_fd, buf, sizeof(buf), 0);
            if (nr < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    http_send_error(client_fd, 408, 0);
                    return;
                }
                if (errno == EINTR) continue;
                return;
            }
            if (nr == 0) return;   /* EOF */

            result = http_parser_feed(&parser, buf, (size_t)nr, &req);

            if (result == PARSE_TOO_LARGE) {
                http_send_error(client_fd, 413, 0);
                return;
            }
            if (result == PARSE_ERROR) {
                http_send_error(client_fd, 400, 0);
                return;
            }
        }

        keep_alive = http_handle_request(client_fd, doc_root, &req);
    }
}

/* ─── Single-threaded accept loop ────────────────────────────────────────── */

static int run_single(int listen_fd, const server_config_t *cfg)
{
    LOG_INFO("Mode: single-threaded");

    int sig_fd = signal_handler_init();

    while (!atomic_load(&g_shutdown_requested)) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);

        /*
         * accept() blocks here until a client connects.
         * It dequeues one entry from the kernel's accept queue and
         * returns a new fd representing that TCP connection.
         */
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (client_fd < 0) {
            if (errno == EINTR) {
                signal_handler_check(sig_fd);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_SYSERR("accept");
            break;
        }

        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        LOG_INFO("Client connected: %s:%d", peer_ip, ntohs(peer.sin_port));

        /* Handle synchronously — blocks until connection closes */
        handle_connection_single(client_fd, cfg->doc_root, cfg->idle_timeout);
        close(client_fd);
    }

    signal_handler_cleanup(sig_fd);
    return 0;
}

/* ─── Thread-pool accept loop ─────────────────────────────────────────────── */

static int run_threadpool(int listen_fd, const server_config_t *cfg)
{
    LOG_INFO("Mode: thread-pool (%d workers, queue=%d)",
             cfg->num_workers, cfg->queue_size);

    tp_config_t tp_cfg = {
        .num_workers  = (size_t)cfg->num_workers,
        .queue_size   = (size_t)cfg->queue_size,
        .doc_root     = cfg->doc_root,
        .idle_timeout = cfg->idle_timeout,
    };

    thread_pool_t *pool = thread_pool_create(&tp_cfg);
    if (!pool) return -1;

    int sig_fd = signal_handler_init();

    while (!atomic_load(&g_shutdown_requested)) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);

        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (client_fd < 0) {
            if (errno == EINTR) {
                signal_handler_check(sig_fd);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_SYSERR("accept");
            break;
        }

        /* Build a job and submit to the thread pool */
        tp_job_t job;
        job.client_fd = client_fd;
        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        snprintf(job.peer_addr, sizeof(job.peer_addr),
                 "%s:%d", peer_ip, ntohs(peer.sin_port));

        LOG_INFO("Client connected: %s (submitting to pool)", job.peer_addr);

        if (thread_pool_submit(pool, &job) < 0) {
            LOG_WARN("Pool rejected job (shutting down) — closing fd=%d",
                     client_fd);
            close(client_fd);
        }
    }

    LOG_INFO("Graceful shutdown: draining thread pool...");
    thread_pool_shutdown(pool);
    signal_handler_cleanup(sig_fd);
    return 0;
}

/* ─── epoll mode ──────────────────────────────────────────────────────────── */

static int run_epoll(int listen_fd, const server_config_t *cfg)
{
    LOG_INFO("Mode: epoll event-loop");

    epoll_server_config_t ecfg = {
        .listen_fd       = listen_fd,
        .doc_root        = cfg->doc_root,
        .idle_timeout    = cfg->idle_timeout,
        .max_connections = cfg->max_connections,
    };
    return epoll_server_run(&ecfg);
}

/* ─── Dispatcher ──────────────────────────────────────────────────────────── */

int server_run(int listen_fd, const server_config_t *cfg)
{
    switch (cfg->mode) {
        case SERVER_MODE_SINGLE:
            return run_single(listen_fd, cfg);
        case SERVER_MODE_THREADPOOL:
            return run_threadpool(listen_fd, cfg);
        case SERVER_MODE_EPOLL:
            return run_epoll(listen_fd, cfg);
        default:
            LOG_ERROR("Unknown server mode");
            return -1;
    }
}
