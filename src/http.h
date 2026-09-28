/*
 * http.h — HTTP Response Builder Interface
 *
 * Constructs well-formed HTTP/1.1 response headers and status lines.
 * All response formatting is done through this module.
 *
 * OS Concept: HTTP is a text protocol layered over TCP. The response
 * must be a correctly formatted byte stream that the client can parse.
 * We use snprintf() throughout to prevent buffer overflows.
 */

#ifndef MICROHTTP_HTTP_H
#define MICROHTTP_HTTP_H

#include "parser.h"
#include "file.h"

#include <sys/types.h>
#include <stddef.h>
#include <time.h>

/* ─── HTTP Status Codes ───────────────────────────────────────────────────── */
#define HTTP_200  200
#define HTTP_206  206
#define HTTP_304  304
#define HTTP_400  400
#define HTTP_403  403
#define HTTP_404  404
#define HTTP_405  405
#define HTTP_408  408
#define HTTP_413  413
#define HTTP_416  416
#define HTTP_500  500
#define HTTP_501  501
#define HTTP_505  505

/* ─── Response context ────────────────────────────────────────────────────── */
typedef struct {
    int              client_fd;     /* socket to write response to */
    const char      *doc_root;      /* document root for file resolution */
    http_request_t  *req;           /* parsed request (may be NULL for errors) */
} http_response_ctx_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/*
 * http_reason — return the standard reason phrase for a status code.
 */
const char *http_reason(int status);

/*
 * http_send_error — send a complete error response with a plain-text body.
 *
 * client_fd: socket to send to
 * status:    HTTP status code (e.g. 404)
 * keep_alive: 1 = Connection: keep-alive, 0 = Connection: close
 */
void http_send_error(int client_fd, int status, int keep_alive);

/*
 * http_format_date — format a time_t as an HTTP-date (RFC 7231 §7.1.1.1).
 *
 * Example: "Sun, 06 Nov 1994 08:49:37 GMT"
 * buf must be at least 30 bytes.
 */
void http_format_date(char *buf, size_t buf_size, time_t t);

/*
 * http_handle_request — fully handle one parsed HTTP request.
 *
 * Resolves the file, checks conditional headers, serves the content.
 * Returns 1 if the connection should be kept alive, 0 if it should close.
 */
int http_handle_request(int client_fd, const char *doc_root,
                        http_request_t *req);

#endif /* MICROHTTP_HTTP_H */
