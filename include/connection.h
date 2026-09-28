/*
 * connection.h — Per-Connection State Machine Interface
 *
 * OS Concept (Week 7, 10):
 *   Each accepted TCP connection is modelled as a state machine.
 *   The state transitions are:
 *
 *   CONN_READING  ──(full request)──>  CONN_PROCESSING
 *   CONN_PROCESSING ──(done)──────>  CONN_WRITING
 *   CONN_WRITING  ──(sent)────────>  CONN_KEEP_ALIVE (or CONN_CLOSING)
 *   CONN_KEEP_ALIVE ───────────────>  CONN_READING   (next request)
 *   CONN_CLOSING  ──(close)───────>  [destroyed]
 *   CONN_TIMEOUT  ──(408 sent)────>  CONN_CLOSING
 *
 * This state machine is shared between the single-threaded, thread-pool,
 * and epoll server modes. Each mode drives the state machine differently.
 */

#ifndef MICROHTTP_CONNECTION_H
#define MICROHTTP_CONNECTION_H

#include "parser.h"

#include <time.h>
#include <sys/types.h>
#include <netinet/in.h>

/* ─── Connection states ───────────────────────────────────────────────────── */
typedef enum {
    CONN_READING    = 0,   /* Waiting for / accumulating HTTP request data */
    CONN_PROCESSING,       /* Request received, generating response         */
    CONN_WRITING,          /* Sending response to client                    */
    CONN_KEEP_ALIVE,       /* Waiting for next request on same connection   */
    CONN_CLOSING,          /* Connection draining / about to close          */
    CONN_TIMEOUT           /* Idle timeout expired                          */
} conn_state_t;

/* ─── Per-connection structure ────────────────────────────────────────────── */
typedef struct {
    int              fd;            /* client socket file descriptor          */
    conn_state_t     state;         /* current state machine state            */
    http_parser_t    parser;        /* incremental HTTP parser                */
    http_request_t   req;           /* last fully parsed request              */
    time_t           last_active;   /* wall-clock time of last I/O activity  */
    char             peer_addr[48]; /* human-readable client IP:port          */
} connection_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/*
 * conn_create — allocate and initialize a connection structure.
 *
 * fd:   accepted client socket
 * addr: remote address from accept()
 * Returns NULL on allocation failure (errno set).
 */
connection_t *conn_create(int fd, const struct sockaddr_in *addr);

/*
 * conn_destroy — close the socket and free the structure.
 * Safe to call with NULL.
 */
void conn_destroy(connection_t *conn);

/*
 * conn_state_str — human-readable state name (for logging).
 */
const char *conn_state_str(conn_state_t state);

#endif /* MICROHTTP_CONNECTION_H */
