/*
 * http.c — HTTP Response Builder Implementation
 *
 * OS Concepts:
 *   - write(2) / send(2): Writing to a socket fd. May return fewer bytes
 *     than requested (partial write) — we loop until all bytes are sent
 *     or the connection is broken.
 *   - stat/fstat: Retrieve file metadata (mtime for Last-Modified / If-Modified-Since).
 *   - HTTP is line-oriented text: snprintf() is the safe way to build headers.
 *   - strptime(): POSIX function to parse HTTP-date strings.
 *   - gmtime_r(): Thread-safe UTC time formatting.
 */

#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include "file.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

/* timegm() is a glibc extension; provide a fallback for macOS/non-glibc */
#ifndef __linux__
static time_t portable_timegm(struct tm *tm)
{
    /* Override TZ to UTC so mktime() treats the struct as UTC */
    time_t t;
    char *tz_save = getenv("TZ");
    setenv("TZ", "", 1);
    tzset();
    t = mktime(tm);
    if (tz_save) setenv("TZ", tz_save, 1);
    else         unsetenv("TZ");
    tzset();
    return t;
}
#  define timegm portable_timegm
#endif

/* ─── Status reason phrases ───────────────────────────────────────────────── */

const char *http_reason(int status)
{
    switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

/* ─── HTTP-date formatting ────────────────────────────────────────────────── */

void http_format_date(char *buf, size_t buf_size, time_t t)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

/* ─── Safe socket write (handles partial writes) ─────────────────────────── */

/*
 * write_all — write exactly 'len' bytes to fd, handling partial writes.
 *
 * OS Concept: A single write() to a socket may transfer fewer bytes than
 * requested if the kernel send buffer is full. We must loop until all
 * bytes are delivered or the connection is broken.
 *
 * Returns 0 on success, -1 on error.
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET) return -1;
            LOG_SYSERR("write");
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/* ─── Error responses ─────────────────────────────────────────────────────── */

void http_send_error(int client_fd, int status, int keep_alive)
{
    const char *reason = http_reason(status);
    char body[256];
    int  body_len = snprintf(body, sizeof(body),
                             "<html><body><h1>%d %s</h1></body></html>\r\n",
                             status, reason);

    char date_buf[48];
    http_format_date(date_buf, sizeof(date_buf), time(NULL));

    char header[512];
    int  hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, reason,
        date_buf,
        body_len,
        keep_alive ? "keep-alive" : "close");

    write_all(client_fd, header, (size_t)hlen);
    write_all(client_fd, body,   (size_t)body_len);
}

/* ─── If-Modified-Since handling ─────────────────────────────────────────── */

/*
 * parse_http_date — parse an HTTP-date string into a time_t.
 * Returns (time_t)-1 on parse failure.
 *
 * HTTP-date format (RFC 7231): "Sun, 06 Nov 1994 08:49:37 GMT"
 */
static time_t parse_http_date(const char *date_str)
{
    struct tm tm_parsed;
    memset(&tm_parsed, 0, sizeof(tm_parsed));

    /* Try RFC 1123 format first */
    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm_parsed) != NULL)
        return timegm(&tm_parsed);

    /* Try RFC 850 format */
    if (strptime(date_str, "%A, %d-%b-%y %H:%M:%S GMT", &tm_parsed) != NULL)
        return timegm(&tm_parsed);

    /* Try ANSI C asctime() format */
    if (strptime(date_str, "%a %b %d %H:%M:%S %Y", &tm_parsed) != NULL)
        return timegm(&tm_parsed);

    return (time_t)-1;
}

/* ─── Main request handler ────────────────────────────────────────────────── */

int http_handle_request(int client_fd, const char *doc_root,
                        http_request_t *req)
{
    /* ── 1. Check HTTP version ──────────────────────────────────────────── */
    if (req->version == HTTP_VERSION_UNKNOWN) {
        LOG_WARN("Unsupported HTTP version");
        http_send_error(client_fd, 505, 0);
        return 0;
    }

    /* ── 2. Check method ────────────────────────────────────────────────── */
    if (req->method != HTTP_METHOD_GET && req->method != HTTP_METHOD_HEAD) {
        if (req->method == HTTP_METHOD_UNKNOWN) {
            http_send_error(client_fd, 501, req->keep_alive);
        } else {
            /* Known method but not supported for static files */
            char hdr[128];
            snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 405 Method Not Allowed\r\n"
                     "Allow: GET, HEAD\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: %s\r\n\r\n",
                     req->keep_alive ? "keep-alive" : "close");
            write_all(client_fd, hdr, strlen(hdr));
        }
        LOG_INFO("%s %s -> 405",
                 http_method_str(req->method), req->path);
        return req->keep_alive;
    }

    /* ── 3. Resolve the file path ───────────────────────────────────────── */
    file_info_t info;
    file_resolve_result_t res = file_resolve_path(doc_root, req->path, &info);

    switch (res) {
        case FILE_RESOLVE_NOT_FOUND:
            LOG_INFO("GET %s -> 404", req->path);
            http_send_error(client_fd, 404, req->keep_alive);
            return req->keep_alive;

        case FILE_RESOLVE_FORBIDDEN:
            LOG_WARN("Path traversal / forbidden: %s", req->path);
            http_send_error(client_fd, 403, req->keep_alive);
            return req->keep_alive;

        case FILE_RESOLVE_ERROR:
            LOG_ERROR("File resolution error for: %s", req->path);
            http_send_error(client_fd, 500, req->keep_alive);
            return req->keep_alive;

        case FILE_RESOLVE_IS_DIR:
            /* Already handled inside file_resolve_path() — shouldn't reach here */
            http_send_error(client_fd, 403, req->keep_alive);
            return req->keep_alive;

        case FILE_RESOLVE_OK:
            break;  /* proceed */
    }

    /* ── 4. If-Modified-Since ──────────────────────────────────────────── */
    if (req->if_modified_since != NULL) {
        time_t client_time = parse_http_date(req->if_modified_since);
        if (client_time != (time_t)-1 && info.mtime <= client_time) {
            /* Resource has not been modified — send 304 */
            char date_buf[48], lm_buf[48];
            http_format_date(date_buf, sizeof(date_buf), time(NULL));
            http_format_date(lm_buf,   sizeof(lm_buf),   info.mtime);

            char hdr[512];
            int n = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 304 Not Modified\r\n"
                "Date: %s\r\n"
                "Last-Modified: %s\r\n"
                "Connection: %s\r\n"
                "\r\n",
                date_buf, lm_buf,
                req->keep_alive ? "keep-alive" : "close");
            write_all(client_fd, hdr, (size_t)n);
            LOG_INFO("%s %s -> 304",
                     http_method_str(req->method), req->path);
            return req->keep_alive;
        }
    }

    /* ── 5. Parse Range header (if any) ────────────────────────────────── */
    byte_range_t range = {0};
    int has_range = 0;

    if (req->range != NULL) {
        has_range = file_parse_range(req->range, info.size, &range);
        if (!has_range) {
            /* Range not satisfiable */
            char hdr[256];
            int n = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */%lld\r\n"
                "Content-Length: 0\r\n"
                "Connection: %s\r\n"
                "\r\n",
                (long long)info.size,
                req->keep_alive ? "keep-alive" : "close");
            write_all(client_fd, hdr, (size_t)n);
            LOG_INFO("%s %s -> 416", http_method_str(req->method), req->path);
            return req->keep_alive;
        }
    }

    /* ── 6. Build response headers ──────────────────────────────────────── */
    char date_buf[48], lm_buf[48];
    http_format_date(date_buf, sizeof(date_buf), time(NULL));
    http_format_date(lm_buf,   sizeof(lm_buf),   info.mtime);

    int    status      = has_range ? 206 : 200;
    off_t  serve_start = has_range ? range.start : 0;
    off_t  serve_end   = has_range ? range.end   : info.size - 1;
    off_t  serve_len   = serve_end - serve_start + 1;

    char hdr[1024];
    int  hlen;

    if (has_range) {
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\n"
            "Date: %s\r\n"
            "Last-Modified: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Connection: %s\r\n"
            "\r\n",
            date_buf, lm_buf,
            info.mime,
            (long long)serve_len,
            (long long)serve_start, (long long)serve_end, (long long)info.size,
            req->keep_alive ? "keep-alive" : "close");
    } else {
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Date: %s\r\n"
            "Last-Modified: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Connection: %s\r\n"
            "\r\n",
            date_buf, lm_buf,
            info.mime,
            (long long)info.size,
            req->keep_alive ? "keep-alive" : "close");
    }

    if (hlen < 0 || hlen >= (int)sizeof(hdr)) {
        LOG_ERROR("Response header too large for: %s", req->path);
        http_send_error(client_fd, 500, 0);
        return 0;
    }

    /* ── 7. Send headers ────────────────────────────────────────────────── */
    if (write_all(client_fd, hdr, (size_t)hlen) != 0) {
        LOG_WARN("Failed to send headers for: %s", req->path);
        return 0;
    }

    /* ── 8. Send body (skip for HEAD) ──────────────────────────────────── */
    if (req->method == HTTP_METHOD_HEAD) {
        LOG_INFO("HEAD %s -> %d", req->path, status);
        return req->keep_alive;
    }

    /* Open the file */
    int fd = open(info.abs_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_SYSERR("open");
        /* Headers already sent; nothing we can do now */
        return 0;
    }

    /* Send the file content */
    ssize_t sent = file_serve_range(fd, client_fd, serve_start, serve_end);
    close(fd);

    if (sent < 0) {
        LOG_WARN("File send error for: %s", req->path);
        return 0;
    }

    LOG_INFO("%s %s -> %d (%lld bytes)",
             http_method_str(req->method), req->path, status, (long long)sent);

    return req->keep_alive;
}
