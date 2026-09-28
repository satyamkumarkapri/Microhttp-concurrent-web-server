/*
 * mime.c — MIME Type Detection Implementation
 *
 * Uses a sorted table of (extension, mime_type) pairs and a linear scan.
 * No external library required.
 */

#include "mime.h"

#include <string.h>
#include <strings.h>
#include <stddef.h>

typedef struct {
    const char *ext;   /* lowercase extension WITHOUT leading dot */
    const char *mime;
} mime_entry_t;

/*
 * Table of known MIME types.
 * Extensions MUST be lowercase — we do a case-insensitive compare.
 */
static const mime_entry_t mime_table[] = {
    { "css",   "text/css" },
    { "gif",   "image/gif" },
    { "htm",   "text/html; charset=utf-8" },
    { "html",  "text/html; charset=utf-8" },
    { "ico",   "image/x-icon" },
    { "jpeg",  "image/jpeg" },
    { "jpg",   "image/jpeg" },
    { "js",    "application/javascript" },
    { "json",  "application/json" },
    { "mp4",   "video/mp4" },
    { "pdf",   "application/pdf" },
    { "png",   "image/png" },
    { "svg",   "image/svg+xml" },
    { "txt",   "text/plain; charset=utf-8" },
    { "webp",  "image/webp" },
    { "woff",  "font/woff" },
    { "woff2", "font/woff2" },
    { "xml",   "application/xml" },
    { NULL, NULL }
};

const char *mime_get(const char *filename)
{
    if (!filename)
        return "application/octet-stream";

    /* Find the last '.' in the filename */
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename)
        return "application/octet-stream";

    const char *ext = dot + 1;  /* pointer to extension without '.' */

    /* Linear scan — table is small enough that binary search is overkill */
    for (size_t i = 0; mime_table[i].ext != NULL; i++) {
        if (strcasecmp(ext, mime_table[i].ext) == 0)
            return mime_table[i].mime;
    }

    return "application/octet-stream";
}
