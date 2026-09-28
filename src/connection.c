/*
 * connection.c — Per-Connection State Machine Implementation
 *
 * OS Concept (Week 2, 7):
 *   - malloc/free: explicit heap allocation. Each connection is a distinct
 *     heap object with a clear owner (the thread or event loop that accepted it).
 *   - close(2): releases the kernel file descriptor. Must be called exactly
 *     once. We use conn_destroy() as the single cleanup path.
 *   - INET_ADDRSTRLEN / inet_ntop(): convert binary IP address to text.
 */


#define _POSIX_C_SOURCE 200809L

#include "connection.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>

connection_t *conn_create(int fd, const struct sockaddr_in *addr)
{
    connection_t *conn = malloc(sizeof(connection_t));
    if (!conn) {
        LOG_SYSERR("malloc (connection_t)");
        return NULL;
    }

    conn->fd         = fd;
    conn->state      = CONN_READING;
    conn->last_active = time(NULL);

    http_parser_init(&conn->parser);
    memset(&conn->req, 0, sizeof(conn->req));

    /* Format peer address as "IP:PORT" for logging */
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    snprintf(conn->peer_addr, sizeof(conn->peer_addr),
             "%s:%d", ip, ntohs(addr->sin_port));

    LOG_DEBUG("Connection created: fd=%d peer=%s", fd, conn->peer_addr);
    return conn;
}

void conn_destroy(connection_t *conn)
{
    if (!conn) return;

    if (conn->fd >= 0) {
        /*
         * OS Concept: close(2) decrements the reference count on the kernel
         * file descriptor. When it reaches zero, the kernel releases the
         * socket, sends TCP FIN to the peer, and frees kernel resources.
         */
        close(conn->fd);
        LOG_DEBUG("Connection closed: fd=%d peer=%s", conn->fd, conn->peer_addr);
        conn->fd = -1;
    }

    free(conn);
}

const char *conn_state_str(conn_state_t state)
{
    switch (state) {
        case CONN_READING:    return "READING";
        case CONN_PROCESSING: return "PROCESSING";
        case CONN_WRITING:    return "WRITING";
        case CONN_KEEP_ALIVE: return "KEEP_ALIVE";
        case CONN_CLOSING:    return "CLOSING";
        case CONN_TIMEOUT:    return "TIMEOUT";
        default:              return "UNKNOWN";
    }
}
