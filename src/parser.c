/*
 * parser.c — HTTP/1.1 Request Parser Implementation
 *
 * Implements an incremental HTTP/1.1 request parser.
 *
 * OS Concept (Week 3): Lexing/parsing, I/O streams.
 *   TCP delivers a byte stream. We accumulate bytes in a fixed-size buffer
 *   and scan for the double-CRLF that terminates the HTTP header section.
 *   We never allocate heap memory during parsing — all storage is in the
 *   caller-supplied http_parser_t (stack or struct member).
 *
 * OS Concept (Week 7): Partial reads.
 *   recv() may return 1 byte, 17 bytes, or the entire request. The parser
 *   handles each call to http_parser_feed() independently and signals
 *   PARSE_INCOMPLETE until the full request is accumulated.
 */



#include "parser.h"
#include "logger.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/* ─── Helpers ─────────────────────────────────────────────────────────────── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * url_decode — percent-decode a URL path.
 * Writes the decoded form into dst (max dst_size bytes including NUL).
 * Returns 0 on success, -1 on invalid percent-encoding.
 *
 * We reject %00 (null byte injection) and invalid sequences.
 */
int url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    const char *s = src;

    while (*s && di < dst_size - 1) {
        if (*s == '%') {
            int h1 = hex_val(s[1]);
            int h2 = (s[1] != '\0') ? hex_val(s[2]) : -1;
            if (h1 < 0 || h2 < 0)
                return -1;            /* invalid percent-encoding */
            int decoded = (h1 << 4) | h2;
            if (decoded == 0)
                return -1;            /* reject %00 — null injection */
            dst[di++] = (char)decoded;
            s += 3;
        } else if (*s == '+') {
            dst[di++] = ' ';
            s++;
        } else {
            dst[di++] = *s++;
        }
    }

    if (*s != '\0' && di >= dst_size - 1)
        return -1;   /* destination too small */

    dst[di] = '\0';
    return 0;
}

/* ─── Method Parsing ──────────────────────────────────────────────────────── */

static http_method_t parse_method(const char *token)
{
    if (strcmp(token, "GET")     == 0) return HTTP_METHOD_GET;
    if (strcmp(token, "HEAD")    == 0) return HTTP_METHOD_HEAD;
    if (strcmp(token, "POST")    == 0) return HTTP_METHOD_POST;
    if (strcmp(token, "PUT")     == 0) return HTTP_METHOD_PUT;
    if (strcmp(token, "DELETE")  == 0) return HTTP_METHOD_DELETE;
    if (strcmp(token, "OPTIONS") == 0) return HTTP_METHOD_OPTIONS;
    if (strcmp(token, "TRACE")   == 0) return HTTP_METHOD_TRACE;
    if (strcmp(token, "CONNECT") == 0) return HTTP_METHOD_CONNECT;
    return HTTP_METHOD_UNKNOWN;
}

const char *http_method_str(http_method_t m)
{
    switch (m) {
        case HTTP_METHOD_GET:     return "GET";
        case HTTP_METHOD_HEAD:    return "HEAD";
        case HTTP_METHOD_POST:    return "POST";
        case HTTP_METHOD_PUT:     return "PUT";
        case HTTP_METHOD_DELETE:  return "DELETE";
        case HTTP_METHOD_OPTIONS: return "OPTIONS";
        case HTTP_METHOD_TRACE:   return "TRACE";
        case HTTP_METHOD_CONNECT: return "CONNECT";
        default:                  return "UNKNOWN";
    }
}

/* ─── Version Parsing ─────────────────────────────────────────────────────── */

static http_version_t parse_version(const char *token)
{
    if (strcmp(token, "HTTP/1.1") == 0) return HTTP_VERSION_11;
    if (strcmp(token, "HTTP/1.0") == 0) return HTTP_VERSION_10;
    return HTTP_VERSION_UNKNOWN;
}

const char *http_version_str(http_version_t v)
{
    switch (v) {
        case HTTP_VERSION_10: return "HTTP/1.0";
        case HTTP_VERSION_11: return "HTTP/1.1";
        default:              return "HTTP/?.?";
    }
}

/* ─── String helpers ──────────────────────────────────────────────────────── */

/* Trim leading whitespace */
static const char *ltrim(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Trim trailing whitespace in-place */
static void rtrim(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

/* ─── Request-line parser ─────────────────────────────────────────────────── */

/*
 * parse_request_line — parse "METHOD SP request-target SP HTTP/version CRLF"
 *
 * line is a NUL-terminated string ending just before the CRLF.
 * Returns 0 on success, -1 on error.
 */
static int parse_request_line(char *line, http_request_t *req)
{
    /* Tokenize by space */
    char *method_tok = strtok(line, " ");
    char *path_tok   = strtok(NULL, " ");
    char *ver_tok    = strtok(NULL, " \r\n");

    if (!method_tok || !path_tok || !ver_tok) {
        LOG_WARN("Malformed request line (missing tokens)");
        return -1;
    }

    /* Extra tokens → error */
    if (strtok(NULL, " \r\n") != NULL) {
        LOG_WARN("Malformed request line (extra tokens)");
        return -1;
    }

    req->method = parse_method(method_tok);
    /* We accept unknown methods — caller checks for 501/405 */

    req->version = parse_version(ver_tok);
    if (req->version == HTTP_VERSION_UNKNOWN) {
        LOG_WARN("Unsupported HTTP version: %s", ver_tok);
        return -1;
    }

    /* Store raw path */
    if (snprintf(req->raw_path, sizeof(req->raw_path), "%s", path_tok)
            >= (int)sizeof(req->raw_path)) {
        LOG_WARN("Request path too long");
        return -1;
    }

    /* Percent-decode path */
    if (url_decode(req->path, sizeof(req->path), path_tok) != 0) {
        LOG_WARN("Invalid percent-encoding in path: %s", path_tok);
        return -1;
    }

    /* Basic path sanity: must start with '/' */
    if (req->path[0] != '/') {
        LOG_WARN("Request path does not start with '/': %s", req->path);
        return -1;
    }

    return 0;
}

/* ─── Header parser ───────────────────────────────────────────────────────── */

/*
 * parse_header_line — parse "Name: value"
 * line is NUL-terminated (CRLF already stripped).
 * Returns 0 on success, -1 on error.
 */
static int parse_header_line(const char *line, http_request_t *req)
{
    if (req->num_headers >= HTTP_MAX_HEADERS) {
        LOG_WARN("Too many headers (max %d)", HTTP_MAX_HEADERS);
        return -1;
    }

    const char *colon = strchr(line, ':');
    if (!colon) {
        LOG_WARN("Malformed header (no colon): %.40s", line);
        return -1;
    }

    http_header_t *h = &req->headers[req->num_headers];

    /* Copy header name, strip trailing spaces */
    size_t name_len = (size_t)(colon - line);
    if (name_len == 0 || name_len >= HTTP_MAX_HEADER_NAME) {
        LOG_WARN("Header name too long or empty");
        return -1;
    }
    memcpy(h->name, line, name_len);
    h->name[name_len] = '\0';
    rtrim(h->name);

    /* Copy header value, strip leading spaces */
    const char *val_start = ltrim(colon + 1);
    if (snprintf(h->value, sizeof(h->value), "%s", val_start)
            >= (int)sizeof(h->value)) {
        LOG_WARN("Header value too long: %.40s", h->name);
        return -1;
    }
    rtrim(h->value);

    req->num_headers++;
    return 0;
}

/* ─── Post-parse: extract well-known headers ──────────────────────────────── */

static void extract_known_headers(http_request_t *req)
{
    req->host              = NULL;
    req->connection        = NULL;
    req->range             = NULL;
    req->if_modified_since = NULL;
    req->content_length    = -1;
    req->keep_alive        = (req->version == HTTP_VERSION_11) ? 1 : 0;

    for (size_t i = 0; i < req->num_headers; i++) {
        const char *name = req->headers[i].name;
        const char *val  = req->headers[i].value;

        if (strcasecmp(name, "Host") == 0) {
            req->host = val;
        } else if (strcasecmp(name, "Connection") == 0) {
            req->connection = val;
            if (strcasecmp(val, "close") == 0)
                req->keep_alive = 0;
            else if (strcasecmp(val, "keep-alive") == 0)
                req->keep_alive = 1;
        } else if (strcasecmp(name, "Content-Length") == 0) {
            char *end;
            req->content_length = strtoll(val, &end, 10);
            if (*end != '\0' || req->content_length < 0)
                req->content_length = -1; /* invalid */
        } else if (strcasecmp(name, "Range") == 0) {
            req->range = val;
        } else if (strcasecmp(name, "If-Modified-Since") == 0) {
            req->if_modified_since = val;
        }
    }
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void http_parser_init(http_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = PARSE_STATE_REQUEST_LINE;
}

parse_result_t http_parser_feed(http_parser_t *p, const char *data,
                                size_t len, http_request_t *req)
{
    /* Guard against accumulated too-large requests */
    if (p->len + len > HTTP_MAX_REQUEST_SIZE) {
        LOG_WARN("HTTP request too large (%zu + %zu bytes)", p->len, len);
        return PARSE_TOO_LARGE;
    }

    /* Append new bytes to the internal buffer */
    memcpy(p->buf + p->len, data, len);
    p->len += len;
    p->buf[p->len] = '\0';  /* always NUL-terminate for safe string ops */

    /*
     * Search for the end-of-headers marker: "\r\n\r\n"
     * HTTP/1.1 RFC 7230 requires CRLF. We also tolerate bare LF for
     * robustness with simple test clients.
     */
    char *header_end = strstr(p->buf, "\r\n\r\n");
    if (!header_end) {
        /* Tolerate bare LF separators (some simple clients omit CR) */
        header_end = strstr(p->buf, "\n\n");
        if (!header_end)
            return PARSE_INCOMPLETE;
        /* Adjust to point past the double LF */
        header_end += 2;  /* skip "\n\n" */
    } else {
        header_end += 4;  /* skip "\r\n\r\n" */
    }

    /*
     * We have a complete header section. Make a mutable copy to tokenize.
     * The header section is p->buf .. header_end (exclusive).
     */
    size_t header_len = (size_t)(header_end - p->buf);
    char work[HTTP_MAX_REQUEST_SIZE + 1];
    memcpy(work, p->buf, header_len);
    work[header_len] = '\0';

    /* Initialize the request structure */
    memset(req, 0, sizeof(*req));

    /*
     * Split into lines on CRLF (or bare LF).
     * First line = request line. Subsequent lines = headers.
     */
    char *saveptr = NULL;
    char *line = strtok_r(work, "\n", &saveptr);
    if (!line) {
        LOG_WARN("Empty HTTP request");
        return PARSE_ERROR;
    }
    rtrim(line);  /* remove trailing CR */

    /* Parse the request line */
    if (parse_request_line(line, req) != 0)
        return PARSE_ERROR;

    /* Parse header lines until blank line */
    while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
        rtrim(line);
        if (line[0] == '\0')
            break;   /* blank line = end of headers */
        if (parse_header_line(line, req) != 0)
            return PARSE_ERROR;
    }

    /* HTTP/1.1 requires a Host header */
    if (req->version == HTTP_VERSION_11) {
        int has_host = 0;
        for (size_t i = 0; i < req->num_headers; i++) {
            if (strcasecmp(req->headers[i].name, "Host") == 0) {
                has_host = 1;
                break;
            }
        }
        if (!has_host) {
            LOG_WARN("HTTP/1.1 request missing required Host header");
            return PARSE_ERROR;
        }
    }

    /* Extract convenience fields */
    extract_known_headers(req);

    p->state = PARSE_STATE_DONE;
    return PARSE_COMPLETE;
}
