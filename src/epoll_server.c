/*
 * epoll_server.c — Linux epoll Event Loop Implementation
 *
 * OS Concepts (Week 10):
 *
 * epoll_create1(EPOLL_CLOEXEC):
 *   Creates the epoll file descriptor. The kernel allocates an event table.
 *
 * epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event):
 *   Registers 'fd' with the epoll instance. The event specifies which
 *   events to watch (EPOLLIN, EPOLLOUT, EPOLLRDHUP, EPOLLERR, EPOLLHUP).
 *   EPOLLET enables edge-triggered mode (fires only on state change).
 *
 * epoll_wait(epfd, events, maxevents, timeout):
 *   Blocks until at least one watched fd is ready, then returns the
 *   list of ready events. timeout=-1 means wait forever.
 *
 * Non-blocking sockets (O_NONBLOCK):
 *   recv()/send() return EAGAIN/EWOULDBLOCK when no data is available
 *   rather than blocking. Essential in an event loop so one slow client
 *   cannot block the entire server.
 *
 * EPOLLRDHUP:
 *   Linux extension — fires when the peer closes the write half of the
 *   TCP connection (sends FIN). More reliable than EPOLLHUP for half-close.
 *
 * Per-connection state:
 *   We store a pointer to the connection_t in the epoll_event.data.ptr
 *   field. This way, when epoll reports an event, we immediately have
 *   the connection context without a hash-table lookup.
 *
 * timerfd integration:
 *   We add a repeating timerfd to epoll so idle-timeout scanning is
 *   driven by the same epoll_wait() call, not a separate thread.
 */


#define _POSIX_C_SOURCE 200809L

#include "epoll_server.h"
#include "connection.h"
#include "http.h"
#include "parser.h"
#include "logger.h"
#include "signal_handler.h"
#include "timer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <sys/epoll.h>

/* ─── Make a socket non-blocking ─────────────────────────────────────────── */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { LOG_SYSERR("fcntl F_GETFL"); return -1; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_SYSERR("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }
    return 0;
}

/* ─── Add / modify / remove from epoll ───────────────────────────────────── */
static int epoll_add(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events;
    ev.data.ptr = ptr;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_SYSERR("epoll_ctl ADD");
        return -1;
    }
    return 0;
}



static void epoll_del(int epfd, int fd)
{
    /* Linux 2.6.9+: event can be NULL */
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

/* ─── Close and destroy a connection ─────────────────────────────────────── */
static void close_connection(int epfd, connection_t *conn)
{
    epoll_del(epfd, conn->fd);
    conn_destroy(conn);
}

/* ─── Handle a readable client socket ────────────────────────────────────── */
/*
 * handle_read — called when epoll reports EPOLLIN on a client socket.
 *
 * In edge-triggered mode (EPOLLET) we must read ALL available data in a loop,
 * because epoll fires only once per state transition.
 *
 * Returns 1 if the connection should remain open, 0 if it should be closed.
 */
static int handle_read(connection_t *conn,
                       const char *doc_root, int idle_timeout)
{
    char buf[4096];
    int  keep_alive = 1;

    for (;;) {
        ssize_t nr = recv(conn->fd, buf, sizeof(buf), 0);
        if (nr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;   /* no more data right now — edge-triggered done */
            if (errno == EINTR) continue;
            LOG_DEBUG("recv error fd=%d: %s", conn->fd, strerror(errno));
            return 0;
        }
        if (nr == 0) {
            LOG_DEBUG("Client EOF fd=%d peer=%s", conn->fd, conn->peer_addr);
            return 0;
        }

        conn->last_active = time(NULL);
        conn->state       = CONN_READING;

        parse_result_t result = http_parser_feed(&conn->parser, buf,
                                                  (size_t)nr, &conn->req);

        if (result == PARSE_INCOMPLETE)
            continue;   /* need more data; read more in this edge-trigger loop */

        if (result == PARSE_TOO_LARGE) {
            http_send_error(conn->fd, 413, 0);
            return 0;
        }
        if (result == PARSE_ERROR) {
            http_send_error(conn->fd, 400, 0);
            return 0;
        }

        /* Full request parsed */
        conn->state = CONN_PROCESSING;
        keep_alive  = http_handle_request(conn->fd, doc_root, &conn->req);
        conn->state = keep_alive ? CONN_KEEP_ALIVE : CONN_CLOSING;

        if (!keep_alive)
            return 0;

        /* Reset parser for next request on the same keep-alive connection */
        http_parser_init(&conn->parser);
        conn->state = CONN_READING;

        /* Reset idle timeout on activity */
        conn->last_active = time(NULL);
        (void)idle_timeout;

        /* Continue reading — there might be a pipelined request already */
    }

    return 1;   /* still open, waiting for more data */
}

/* ─── Scan for idle connections ───────────────────────────────────────────── */
/*
 * We keep a simple linked-list-style approach: store all active connections
 * in a flat array indexed by fd. Max fd is bounded by max_connections.
 */
#define MAX_FD 65536   /* practical limit on most Linux systems */

static connection_t *conn_table[MAX_FD];  /* indexed by client fd */

static void scan_idle(int epfd, int idle_timeout)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_FD; i++) {
        connection_t *c = conn_table[i];
        if (!c) continue;

        double elapsed = difftime(now, c->last_active);
        if (elapsed >= idle_timeout) {
            LOG_INFO("Idle timeout: fd=%d peer=%s (%.0fs idle)",
                     c->fd, c->peer_addr, elapsed);
            http_send_error(c->fd, 408, 0);
            conn_table[i] = NULL;
            close_connection(epfd, c);
        }
    }
}

/* ─── Main epoll event loop ───────────────────────────────────────────────── */

int epoll_server_run(const epoll_server_config_t *cfg)
{
    memset(conn_table, 0, sizeof(conn_table));

    /* Create epoll instance */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { LOG_SYSERR("epoll_create1"); return -1; }

    /* Set listen fd to non-blocking */
    if (set_nonblocking(cfg->listen_fd) < 0) {
        close(epfd); return -1;
    }

    /* Add listener to epoll (use fd as sentinel pointer) */
    /* We use a special sentinel to distinguish listener from client events */
    static int sentinel_listen = 0;
    if (epoll_add(epfd, cfg->listen_fd, EPOLLIN, &sentinel_listen) < 0) {
        close(epfd); return -1;
    }

    /* Set up timerfd for idle scans */
    int tfd = timer_create_idle(cfg->idle_timeout > 2 ?
                                cfg->idle_timeout / 2 : 1);
    static int sentinel_timer = 0;
    if (tfd >= 0) {
        epoll_add(epfd, tfd, EPOLLIN, &sentinel_timer);
    }

    /* Set up signalfd */
    int sfd = signal_handler_init();
    static int sentinel_signal = 0;
    if (sfd >= 0) {
        epoll_add(epfd, sfd, EPOLLIN, &sentinel_signal);
    }

    LOG_INFO("epoll event loop started (max_events=512)");

    #define MAX_EVENTS 512
    struct epoll_event events[MAX_EVENTS];
    int active_conns = 0;

    while (!atomic_load(&g_shutdown_requested)) {
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 1000 /* ms */);
        if (nev < 0) {
            if (errno == EINTR) continue;
            LOG_SYSERR("epoll_wait");
            break;
        }

        for (int i = 0; i < nev; i++) {
            void    *ptr = events[i].data.ptr;
            uint32_t ev  = events[i].events;

            /* ── Signal fd ── */
            if (ptr == &sentinel_signal) {
                signal_handler_check(sfd);
                break;
            }

            /* ── Timer fd ── */
            if (ptr == &sentinel_timer) {
                timer_read(tfd);
                scan_idle(epfd, cfg->idle_timeout);
                continue;
            }

            /* ── Listen fd — new connection ── */
            if (ptr == &sentinel_listen) {
                /* Accept all pending connections (edge-triggered not set
                 * on listener, but we loop anyway for robustness) */
                for (;;) {
                    struct sockaddr_in peer;
                    socklen_t plen = sizeof(peer);
                    int cfd = accept4(cfg->listen_fd,
                                      (struct sockaddr *)&peer, &plen,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG_SYSERR("accept4");
                        break;
                    }

                    if (active_conns >= cfg->max_connections) {
                        LOG_WARN("Max connections reached (%d) — rejecting fd=%d",
                                 cfg->max_connections, cfd);
                        close(cfd);
                        continue;
                    }

                    connection_t *conn = conn_create(cfd, &peer);
                    if (!conn) { close(cfd); continue; }

                    if (cfd < MAX_FD) {
                        conn_table[cfd] = conn;
                    } else {
                        LOG_WARN("fd %d exceeds MAX_FD table", cfd);
                        conn_destroy(conn);
                        continue;
                    }

                    /*
                     * Register with epoll:
                     * EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLHUP | EPOLLERR
                     *
                     * EPOLLET: edge-triggered — fire only on transitions.
                     * EPOLLRDHUP: detect peer close/half-close.
                     */
                    uint32_t want = EPOLLIN | EPOLLRDHUP |
                                    EPOLLET | EPOLLHUP | EPOLLERR;
                    if (epoll_add(epfd, cfd, want, conn) < 0) {
                        conn_table[cfd] = NULL;
                        conn_destroy(conn);
                        continue;
                    }

                    active_conns++;
                    LOG_INFO("Client connected: fd=%d peer=%s (active=%d)",
                             cfd, conn->peer_addr, active_conns);
                }
                continue;
            }

            /* ── Client fd event ── */
            connection_t *conn = (connection_t *)ptr;
            if (!conn) continue;

            int should_close = 0;

            if (ev & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                should_close = 1;
            } else if (ev & EPOLLIN) {
                int ok = handle_read(conn,
                                     cfg->doc_root, cfg->idle_timeout);
                if (!ok) should_close = 1;
            }

            if (should_close) {
                int fd = conn->fd;
                if (fd >= 0 && fd < MAX_FD)
                    conn_table[fd] = NULL;
                close_connection(epfd, conn);
                active_conns--;
                LOG_DEBUG("Connection closed (active=%d)", active_conns);
            }
        }
    }

    LOG_INFO("epoll event loop shutting down — closing %d connections",
             active_conns);

    /* Close all remaining connections */
    for (int i = 0; i < MAX_FD; i++) {
        if (conn_table[i]) {
            close_connection(epfd, conn_table[i]);
            conn_table[i] = NULL;
        }
    }

    if (tfd >= 0) close(tfd);
    if (sfd >= 0) signal_handler_cleanup(sfd);
    close(epfd);

    LOG_INFO("epoll server stopped");
    return 0;
}

#else /* !__linux__ */

int epoll_server_run(const epoll_server_config_t *cfg)
{
    (void)cfg;
    fprintf(stderr,
        "[ERROR] epoll mode is Linux-only.\n"
        "        Compile and run on Ubuntu Linux to use --mode epoll.\n");
    return -1;
}

#endif /* __linux__ */
