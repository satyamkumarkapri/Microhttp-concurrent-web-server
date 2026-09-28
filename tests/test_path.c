/*
 * test_path.c — Unit Tests for Path Resolution and Security
 *
 * Tests file_resolve_path() in isolation.
 * Verifies directory traversal protection, normal resolution, and 404 handling.
 *
 * OS Concept (Week 5, 9):
 *   realpath(3) is a system call wrapper that walks the VFS to produce a
 *   canonical path. Testing this separately ensures the security boundary
 *   is correct before integrating with the HTTP server.
 *
 * Build: gcc -std=c11 -Wall -g -o build/test_path \
 *            tests/test_path.c src/file.c src/mime.c src/logger.c -Iinclude
 */


/* Ensure mkdtemp and realpath are available:
 * macOS needs _DARWIN_C_SOURCE or _GNU_SOURCE (already set by Makefile).
 * Linux just needs POSIX 2008.  The Makefile sets -D_GNU_SOURCE which covers both. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file.h"
#include "logger.h"

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { tests_run++; fprintf(stdout, "  [ RUN ] %s\n", name); } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            fprintf(stderr, "    [FAIL] %s:%d: expected %d == %d\n", \
                    __FILE__, __LINE__, (int)(a), (int)(b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define PASS() do { tests_passed++; fprintf(stdout, "  [ OK  ] \n"); } while(0)

/* ─── Setup: create a temporary doc root for testing ─────────────────────── */

static char test_root[256];

static void setup_test_root(void)
{
    /* Create a unique temp directory using PID (portable, no mkdtemp needed) */
    snprintf(test_root, sizeof(test_root), "/tmp/microhttp_test_%d", (int)getpid());
    if (mkdir(test_root, 0755) != 0) {
        perror("mkdir");
        exit(EXIT_FAILURE);
    }

    /* Create public/index.html */
    char path[512];
    snprintf(path, sizeof(path), "%s/index.html", test_root);
    FILE *f = fopen(path, "w");
    if (f) { fputs("<html>Hello</html>", f); fclose(f); }

    snprintf(path, sizeof(path), "%s/test.txt", test_root);
    f = fopen(path, "w");
    if (f) { fputs("test content", f); fclose(f); }

    /* Create a subdirectory */
    snprintf(path, sizeof(path), "%s/subdir", test_root);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/subdir/page.html", test_root);
    f = fopen(path, "w");
    if (f) { fputs("<html>Sub</html>", f); fclose(f); }

    fprintf(stdout, "  [SETUP] Test root: %s\n", test_root);
}

static void cleanup_test_root(void)
{
    /* Simple cleanup */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_root);
    system(cmd);
}

/* ─── Tests ────────────────────────────────────────────────────────────────── */

static void test_root_index(void)
{
    TEST("Root '/' resolves to index.html");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/", &info);
    EXPECT_EQ(r, FILE_RESOLVE_OK);
    fprintf(stdout, "  [ OK  ] resolved: %s\n", info.abs_path);
    tests_passed++;
}

static void test_direct_file(void)
{
    TEST("Direct file /test.txt");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/test.txt", &info);
    EXPECT_EQ(r, FILE_RESOLVE_OK);
    EXPECT_EQ(info.size > 0, 1);
    PASS();
}

static void test_not_found(void)
{
    TEST("Missing file returns NOT_FOUND");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/no_such_file.txt", &info);
    EXPECT_EQ(r, FILE_RESOLVE_NOT_FOUND);
    PASS();
}

static void test_traversal_dotdot(void)
{
    TEST("Path traversal /../etc/passwd → FORBIDDEN");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/../etc/passwd", &info);
    /* Must be NOT_FOUND or FORBIDDEN — never OK */
    if (r == FILE_RESOLVE_OK) {
        fprintf(stderr, "    [FAIL] traversal was NOT blocked! path=%s\n",
                info.abs_path);
        tests_failed++;
    } else {
        fprintf(stdout, "  [ OK  ] blocked (result=%d)\n", (int)r);
        tests_passed++;
    }
}

static void test_traversal_encoded(void)
{
    TEST("Encoded traversal /..%%2F..%%2Fetc/passwd → percent-decoded then rejected");
    /*
     * The parser percent-decodes the path. So /..%2F..%2Fetc/passwd becomes
     * /../../etc/passwd before reaching file_resolve_path().
     * This test verifies that the decoded form is also rejected.
     */
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root,
                                                "/../../etc/passwd", &info);
    if (r == FILE_RESOLVE_OK) {
        fprintf(stderr, "    [FAIL] traversal NOT blocked!\n");
        tests_failed++;
    } else {
        fprintf(stdout, "  [ OK  ] blocked (result=%d)\n", (int)r);
        tests_passed++;
    }
}

static void test_subdirectory_file(void)
{
    TEST("Subdirectory file /subdir/page.html");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/subdir/page.html", &info);
    EXPECT_EQ(r, FILE_RESOLVE_OK);
    PASS();
}

static void test_mime_html(void)
{
    TEST("MIME type for .html");
    file_info_t info;
    file_resolve_result_t r = file_resolve_path(test_root, "/index.html", &info);
    EXPECT_EQ(r, FILE_RESOLVE_OK);
    if (strstr(info.mime, "text/html") == NULL) {
        fprintf(stderr, "    [FAIL] expected text/html, got: %s\n", info.mime);
        tests_failed++;
        return;
    }
    PASS();
}

static void test_range_parse_full(void)
{
    TEST("Range parse: bytes=0-99");
    byte_range_t range;
    int ok = file_parse_range("bytes=0-99", 1000, &range);
    EXPECT_EQ(ok, 1);
    EXPECT_EQ((int)range.start, 0);
    EXPECT_EQ((int)range.end,   99);
    PASS();
}

static void test_range_parse_open_end(void)
{
    TEST("Range parse: bytes=500- (open end)");
    byte_range_t range;
    int ok = file_parse_range("bytes=500-", 1000, &range);
    EXPECT_EQ(ok, 1);
    EXPECT_EQ((int)range.start, 500);
    EXPECT_EQ((int)range.end,   999);
    PASS();
}

static void test_range_parse_suffix(void)
{
    TEST("Range parse: bytes=-100 (last 100)");
    byte_range_t range;
    int ok = file_parse_range("bytes=-100", 1000, &range);
    EXPECT_EQ(ok, 1);
    EXPECT_EQ((int)range.start, 900);
    EXPECT_EQ((int)range.end,   999);
    PASS();
}

static void test_range_parse_invalid(void)
{
    TEST("Range parse: invalid range rejected");
    byte_range_t range;
    int ok = file_parse_range("bytes=900-100", 1000, &range);  /* start > end */
    EXPECT_EQ(ok, 0);
    PASS();
}

static void test_range_parse_unsatisfiable(void)
{
    TEST("Range parse: start >= file_size → unsatisfiable");
    byte_range_t range;
    int ok = file_parse_range("bytes=2000-2999", 1000, &range);
    EXPECT_EQ(ok, 0);
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    g_log_level = LOG_LEVEL_NONE;  /* suppress logger output during tests */

    setup_test_root();

    fprintf(stdout, "\n═══ Path Resolution Unit Tests ═══════════════════════\n\n");

    test_root_index();
    test_direct_file();
    test_not_found();
    test_traversal_dotdot();
    test_traversal_encoded();
    test_subdirectory_file();
    test_mime_html();
    test_range_parse_full();
    test_range_parse_open_end();
    test_range_parse_suffix();
    test_range_parse_invalid();
    test_range_parse_unsatisfiable();

    cleanup_test_root();

    fprintf(stdout,
            "\n═══ Results: %d/%d passed",
            tests_passed, tests_run);
    if (tests_failed > 0)
        fprintf(stdout, ", %d FAILED", tests_failed);
    fprintf(stdout, " ════════════════\n\n");

    return (tests_failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
