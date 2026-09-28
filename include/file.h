/*
 * file.h — Static File Resolution and Serving Interface
 *
 * OS Concept (Week 5, 9):
 *   - Path canonicalization prevents directory traversal attacks.
 *   - stat/fstat retrieves inode metadata (size, mtime) without opening.
 *   - sendfile() transfers file data in kernel space (zero-copy on Linux).
 *   - File offsets allow Range requests without reading from byte 0.
 *
 * Security: All path construction goes through file_resolve_path() which
 * calls realpath() and verifies the result is under the document root.
 * This is the single chokepoint for path safety.
 */

#ifndef MICROHTTP_FILE_H
#define MICROHTTP_FILE_H

#include <sys/stat.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

/* ─── Result of a path resolution attempt ─────────────────────────────────── */
typedef enum {
    FILE_RESOLVE_OK = 0,
    FILE_RESOLVE_NOT_FOUND,      /* path does not exist under doc root */
    FILE_RESOLVE_FORBIDDEN,      /* path exists but is outside doc root */
    FILE_RESOLVE_IS_DIR,         /* path is a directory (use index.html) */
    FILE_RESOLVE_ERROR           /* system error (errno set) */
} file_resolve_result_t;

/* ─── Metadata about a resolved file ─────────────────────────────────────── */
typedef struct {
    char        abs_path[4096];  /* absolute canonical filesystem path */
    off_t       size;            /* file size in bytes */
    time_t      mtime;           /* last modification time (UNIX epoch) */
    const char *mime;            /* MIME type string (static, no free) */
} file_info_t;

/* ─── Range specifier ─────────────────────────────────────────────────────── */
typedef struct {
    off_t start;   /* inclusive byte offset */
    off_t end;     /* inclusive byte offset */
    int   valid;   /* 1 = parsed successfully */
} byte_range_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/*
 * file_resolve_path — resolve a URL path to a safe filesystem path.
 *
 * doc_root:  absolute path to the document root directory.
 * url_path:  percent-decoded URL path (must start with '/').
 * info:      output — populated on FILE_RESOLVE_OK.
 *
 * Internally calls realpath() to canonicalize and then checks that the
 * result is a prefix of doc_root. Rejects directory traversal.
 */
file_resolve_result_t file_resolve_path(const char *doc_root,
                                        const char *url_path,
                                        file_info_t *info);

/*
 * file_serve_range — send [start..end] bytes of a file over a socket.
 *
 * Uses sendfile() on Linux (zero-copy). Falls back to read/write on error.
 * Handles EINTR and partial progress correctly.
 *
 * fd:      open file descriptor (read-only, positioned doesn't matter)
 * client:  socket fd to write to
 * start:   byte offset (inclusive)
 * end:     byte offset (inclusive)
 *
 * Returns bytes written, or -1 on error.
 */
ssize_t file_serve_range(int fd, int client, off_t start, off_t end);

/*
 * file_parse_range — parse a "Range: bytes=start-end" header value.
 *
 * file_size: total file size (needed to resolve suffix ranges like "-100")
 * range_hdr: the value portion of the Range header (e.g. "bytes=0-99")
 * out:       populated on success
 *
 * Returns 1 on success, 0 on invalid/unsatisfiable range.
 */
int file_parse_range(const char *range_hdr, off_t file_size,
                     byte_range_t *out);

#endif /* MICROHTTP_FILE_H */
