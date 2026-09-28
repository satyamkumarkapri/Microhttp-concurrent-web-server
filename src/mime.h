/*
 * mime.h — MIME Type Detection Interface
 *
 * Maps file extensions to MIME type strings without any external library.
 *
 * OS Concept: Static file serving requires the HTTP server to tell the
 * browser how to interpret the body. MIME type detection is purely a
 * string-matching problem performed in user space.
 */

#ifndef MICROHTTP_MIME_H
#define MICROHTTP_MIME_H

/*
 * mime_get — return the MIME type string for a given filename.
 *
 * Inspects the file extension (the part after the last '.').
 * Returns "application/octet-stream" for unknown or missing extensions.
 *
 * The returned pointer is always a string literal (never needs free).
 */
const char *mime_get(const char *filename);

#endif /* MICROHTTP_MIME_H */
