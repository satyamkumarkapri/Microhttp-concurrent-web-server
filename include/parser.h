/*
 * parser.h — HTTP/1.1 Request Parser Interface
 *
 * Implements an incremental (streaming) parser for HTTP/1.1 requests.
 * The parser does NOT assume that a complete request arrives in one recv().
 *
 * OS Concept: TCP is a stream protocol. recv() returns whatever bytes are
 * available in the kernel receive buffer at that moment — which may be
 * less than one full HTTP request, or more than one. The parser must handle
 * both cases correctly.
 *
 * Design:
 *   - Caller feeds bytes via http_parser_feed().
 *   - Parser returns PARSE_INCOMPLETE until a full request is ready.
 *   - Parser returns PARSE_COMPLETE with a populated http_request_t.
 *   - Parser returns PARSE_ERROR on malformed input.
 *   - All strings in http_request_t point into the parser's internal buffer.
 */

#ifndef MICROHTTP_PARSER_H
#define MICROHTTP_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* ─── Limits ─────────────────────────────────────────────────────────────── */
#define HTTP_MAX_REQUEST_SIZE  (16 * 1024)  /* 16 KB max total request */
#define HTTP_MAX_METHOD_LEN    16
#define HTTP_MAX_PATH_LEN      2048
#define HTTP_MAX_VERSION_LEN   16
#define HTTP_MAX_HEADERS       32
#define HTTP_MAX_HEADER_NAME   64
#define HTTP_MAX_HEADER_VALUE  1024

/* ─── HTTP Method ─────────────────────────────────────────────────────────── */
typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_TRACE,
    HTTP_METHOD_CONNECT
} http_method_t;

/* ─── HTTP Version ────────────────────────────────────────────────────────── */
typedef enum {
    HTTP_VERSION_UNKNOWN = 0,
    HTTP_VERSION_10,
    HTTP_VERSION_11
} http_version_t;

/* ─── A Single Parsed Header ──────────────────────────────────────────────── */
typedef struct {
    char name[HTTP_MAX_HEADER_NAME];
    char value[HTTP_MAX_HEADER_VALUE];
} http_header_t;

/* ─── Fully Parsed HTTP Request ───────────────────────────────────────────── */
typedef struct {
    http_method_t  method;
    http_version_t version;
    char           path[HTTP_MAX_PATH_LEN];       /* decoded request target */
    char           raw_path[HTTP_MAX_PATH_LEN];   /* raw (encoded) target   */

    http_header_t  headers[HTTP_MAX_HEADERS];
    size_t         num_headers;

    /* Derived convenience fields (set after full parse) */
    const char    *host;             /* points into headers array, or NULL */
    const char    *connection;       /* Connection: header value, or NULL  */
    const char    *range;            /* Range: header value, or NULL       */
    const char    *if_modified_since;/* If-Modified-Since: value, or NULL  */
    long long      content_length;   /* -1 if not present                  */

    int            keep_alive;       /* 1 = persistent, 0 = close          */
} http_request_t;

/* ─── Parser State Machine ────────────────────────────────────────────────── */
typedef enum {
    PARSE_STATE_REQUEST_LINE = 0,
    PARSE_STATE_HEADERS,
    PARSE_STATE_DONE
} parse_state_t;

/* ─── Parser Context ─────────────────────────────────────────────────────── */
typedef struct {
    char          buf[HTTP_MAX_REQUEST_SIZE + 1]; /* raw bytes received */
    size_t        len;                            /* bytes in buf        */
    parse_state_t state;
    int           error;                          /* 1 if parse error    */
} http_parser_t;

/* ─── Parse Result ────────────────────────────────────────────────────────── */
typedef enum {
    PARSE_INCOMPLETE = 0,   /* need more data */
    PARSE_COMPLETE,         /* request fully parsed, req is populated */
    PARSE_ERROR,            /* malformed request */
    PARSE_TOO_LARGE         /* request exceeded HTTP_MAX_REQUEST_SIZE */
} parse_result_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/* Initialize / reset a parser context */
void http_parser_init(http_parser_t *p);

/*
 * Feed new bytes into the parser.
 * data: pointer to received bytes
 * len:  number of bytes
 * req:  output — populated when PARSE_COMPLETE is returned
 *
 * Returns one of the parse_result_t values.
 */
parse_result_t http_parser_feed(http_parser_t *p, const char *data,
                                size_t len, http_request_t *req);

/* Return a human-readable name for an http_method_t */
const char *http_method_str(http_method_t m);

/* Return a human-readable name for an http_version_t */
const char *http_version_str(http_version_t v);

/*
 * Percent-decode a URL path in-place.
 * Returns 0 on success, -1 if the encoding is invalid.
 */
int url_decode(char *dst, size_t dst_size, const char *src);

#endif /* MICROHTTP_PARSER_H */
