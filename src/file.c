/*
 * file.c — Static File Resolution and Serving Implementation
 *
 * OS Concepts:
 *   - realpath(3) / canonicalize_file_name(3): resolves symlinks and ".."
 *     components by consulting the kernel's VFS, returning the true
 *     absolute path. This is the correct way to prevent path traversal.
 *   - stat(2): query inode metadata without opening the file.
 *   - open(2) / read(2) / write(2): POSIX file I/O.
 *   - sendfile(2): Linux kernel call that moves data from a file fd to a
 *     socket fd entirely in kernel space ("zero-copy"). Avoids a round-trip
 *     through user space, reducing CPU and memory bandwidth.
 *   - off_t / lseek(2): file position / offset type. Used for Range.
 */


#define _POSIX_C_SOURCE 200809L

#include "file.h"
#include "mime.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * On Linux, sys/sendfile.h provides sendfile().
 * On macOS, sendfile has a different signature — we use the fallback there.
 * The Makefile defines LINUX_BUILD when compiling on Linux.
 */
#ifdef __linux__
#  include <sys/sendfile.h>
#  define HAVE_SENDFILE 1
#else
#  define HAVE_SENDFILE 0
#endif

/* ─── Path Resolution ────────────────────────────────────────────────────── */

file_resolve_result_t file_resolve_path(const char *doc_root,
                                        const char *url_path,
                                        file_info_t *info)
{
    /*
     * Step 1: Build a raw candidate path by concatenating doc_root + url_path.
     *
     * Example: doc_root="/srv/public", url_path="/../../etc/passwd"
     *          raw = "/srv/public/../../etc/passwd"
     */
    char raw[4096];
    int n = snprintf(raw, sizeof(raw), "%s%s", doc_root, url_path);
    if (n < 0 || n >= (int)sizeof(raw)) {
        LOG_WARN("Path too long: %s + %s", doc_root, url_path);
        return FILE_RESOLVE_FORBIDDEN;
    }

    /*
     * Step 2: If url_path == "/" serve index.html automatically.
     */
    if (strcmp(url_path, "/") == 0) {
        int r = snprintf(raw, sizeof(raw), "%s/index.html", doc_root);
        if (r < 0 || r >= (int)sizeof(raw))
            return FILE_RESOLVE_FORBIDDEN;
    }

    /*
     * Step 3: Call realpath() to resolve symlinks and ".." traversal.
     *
     * realpath(3) calls stat(2) on every component of the path to resolve
     * symbolic links and normalise "." and "..". It returns NULL if any
     * component does not exist.
     *
     * IMPORTANT: realpath() requires that the file actually exists. If it
     * doesn't exist, we return NOT_FOUND (not FORBIDDEN) to avoid leaking
     * information about path structure via timing differences.
     */
    char resolved[4096];
    if (realpath(raw, resolved) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR)
            return FILE_RESOLVE_NOT_FOUND;
        if (errno == EACCES)
            return FILE_RESOLVE_FORBIDDEN;
        LOG_SYSERR("realpath");
        return FILE_RESOLVE_ERROR;
    }

    /*
     * Step 4: Verify that resolved path starts with the CANONICAL doc_root.
     *
     * CRITICAL: doc_root may be a relative path (e.g. "./public"). We must
     * canonicalize it too, otherwise the prefix comparison fails.
     *
     * We also check for a '/' after the prefix to avoid matching
     * "/srv/public_evil" when doc_root is "/srv/public".
     */
    char canonical_root[4096];
    if (realpath(doc_root, canonical_root) == NULL) {
        LOG_SYSERR("realpath (doc_root)");
        return FILE_RESOLVE_ERROR;
    }

    size_t root_len = strlen(canonical_root);
    if (strncmp(resolved, canonical_root, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        LOG_WARN("Path traversal attempt rejected: %s -> %s", url_path, resolved);
        return FILE_RESOLVE_FORBIDDEN;
    }

    /*
     * Step 5: stat() the file to get size and mtime.
     */
    struct stat st;
    if (stat(resolved, &st) != 0) {
        if (errno == ENOENT) return FILE_RESOLVE_NOT_FOUND;
        if (errno == EACCES) return FILE_RESOLVE_FORBIDDEN;
        LOG_SYSERR("stat");
        return FILE_RESOLVE_ERROR;
    }

    /* Directories: try index.html inside them */
    if (S_ISDIR(st.st_mode)) {
        char idx[4096];
        int r = snprintf(idx, sizeof(idx), "%s/index.html", resolved);
        if (r < 0 || r >= (int)sizeof(idx))
            return FILE_RESOLVE_FORBIDDEN;

        /* Verify index.html is still under doc root (it always is since
         * resolved is already verified, but be explicit) */
        if (stat(idx, &st) != 0) {
            if (errno == ENOENT) return FILE_RESOLVE_NOT_FOUND;
            return FILE_RESOLVE_FORBIDDEN;
        }
        memcpy(resolved, idx, sizeof(resolved));
    }

    /* Must be a regular file */
    if (!S_ISREG(st.st_mode))
        return FILE_RESOLVE_FORBIDDEN;

    /* Populate output */
    memcpy(info->abs_path, resolved, sizeof(info->abs_path));
    info->size  = st.st_size;
    info->mtime = st.st_mtime;
    info->mime  = mime_get(resolved);

    return FILE_RESOLVE_OK;
}

/* ─── Fallback: read/write loop ───────────────────────────────────────────── */

/*
 * send_file_rw — send bytes [start..end] using read()+write().
 *
 * Used on non-Linux platforms or when sendfile() is unavailable/fails.
 * Uses a stack-allocated buffer; never loads the whole file into memory.
 */
static ssize_t send_file_rw(int fd, int client, off_t start, off_t end)
{
    if (lseek(fd, start, SEEK_SET) == (off_t)-1) {
        LOG_SYSERR("lseek");
        return -1;
    }

    char buf[65536];   /* 64 KB read/write buffer — fits in one page table */
    ssize_t total = 0;
    off_t   remaining = end - start + 1;

    while (remaining > 0) {
        size_t to_read = (remaining < (off_t)sizeof(buf))
                         ? (size_t)remaining : sizeof(buf);

        ssize_t nr = read(fd, buf, to_read);
        if (nr < 0) {
            if (errno == EINTR) continue;  /* interrupted by signal, retry */
            LOG_SYSERR("read (file)");
            return -1;
        }
        if (nr == 0)
            break;   /* EOF — file shorter than expected */

        /* Write all bytes we read — handle partial write */
        ssize_t written = 0;
        while (written < nr) {
            ssize_t nw = write(client, buf + written, (size_t)(nr - written));
            if (nw < 0) {
                if (errno == EINTR) continue;
                if (errno == EPIPE || errno == ECONNRESET)
                    return total;   /* client disconnected — not an error */
                LOG_SYSERR("write (socket)");
                return -1;
            }
            written += nw;
            total   += nw;
        }
        remaining -= nr;
    }

    return total;
}

/* ─── sendfile() wrapper ──────────────────────────────────────────────────── */

ssize_t file_serve_range(int fd, int client, off_t start, off_t end)
{
    if (start < 0 || end < start) {
        LOG_WARN("file_serve_range: invalid range [%lld, %lld]",
                 (long long)start, (long long)end);
        return -1;
    }

#if HAVE_SENDFILE
    /*
     * Linux sendfile(2):
     *   sendfile(out_fd, in_fd, &offset, count)
     *
     * 'offset' is an in/out parameter: the kernel updates it after each
     * partial transfer. Loop until all bytes are sent.
     *
     * sendfile() may return EAGAIN on a non-blocking socket (epoll mode)
     * or EINVAL if the file/socket combination isn't supported (e.g. some
     * encrypted file systems). Fall back to read/write in those cases.
     */
    off_t   count = end - start + 1;
    ssize_t total = 0;
    off_t offset = start;
    while (count > 0) {
        ssize_t sent = sendfile(client, fd, &offset, (size_t)count);
        if (sent < 0) {
            if (errno == EINTR)   continue;
            if (errno == EAGAIN)  break;   /* would block; caller handles */
            if (errno == EINVAL || errno == ENOSYS) {
                /* sendfile() not supported for this fd — use read/write */
                LOG_DEBUG("sendfile() not supported, falling back to read/write");
                return send_file_rw(fd, client, start + total, end);
            }
            if (errno == EPIPE || errno == ECONNRESET)
                return total;   /* client disconnected */
            LOG_SYSERR("sendfile");
            return -1;
        }
        if (sent == 0) break;  /* EOF */
        total += sent;
        count -= sent;
    }
    return total;
#else
    /* macOS or other POSIX: always use read/write */
    return send_file_rw(fd, client, start, end);
#endif
}

/* ─── Range Parsing ───────────────────────────────────────────────────────── */

/*
 * file_parse_range — parse "Range: bytes=start-end"
 *
 * Supported forms:
 *   bytes=0-499      → bytes 0 to 499 inclusive
 *   bytes=500-       → bytes 500 to EOF
 *   bytes=-500       → last 500 bytes
 *
 * Returns 1 on success, 0 on invalid or unsatisfiable range.
 */
int file_parse_range(const char *range_hdr, off_t file_size,
                     byte_range_t *out)
{
    if (!range_hdr || !out || file_size <= 0)
        return 0;

    out->valid = 0;

    /* Must start with "bytes=" */
    if (strncmp(range_hdr, "bytes=", 6) != 0)
        return 0;

    const char *spec = range_hdr + 6;

    /* Reject multi-range (commas) — not implemented */
    if (strchr(spec, ',') != NULL)
        return 0;

    const char *dash = strchr(spec, '-');
    if (!dash)
        return 0;

    off_t start, end;
    char *endptr;

    if (dash == spec) {
        /* Suffix form: bytes=-N → last N bytes */
        long long suffix = strtoll(spec, &endptr, 10);
        /* spec itself is '-', so strtoll will get 0 or negative */
        /* Actually parse what's after the dash */
        suffix = strtoll(dash + 1, &endptr, 10);
        if (*endptr != '\0' || suffix <= 0)
            return 0;
        start = file_size - suffix;
        end   = file_size - 1;
        if (start < 0) start = 0;
    } else if (*(dash + 1) == '\0') {
        /* Open end form: bytes=N- */
        start = (off_t)strtoll(spec, &endptr, 10);
        if (endptr != dash || start < 0)
            return 0;
        end = file_size - 1;
    } else {
        /* Full form: bytes=N-M */
        start = (off_t)strtoll(spec, &endptr, 10);
        if (endptr != dash || start < 0)
            return 0;
        end = (off_t)strtoll(dash + 1, &endptr, 10);
        if (*endptr != '\0' || end < start)
            return 0;
    }

    /* Clamp end to file size */
    if (end >= file_size) end = file_size - 1;

    /* Unsatisfiable if start > end or start >= file_size */
    if (start > end || start >= file_size)
        return 0;

    out->start = start;
    out->end   = end;
    out->valid = 1;
    return 1;
}
