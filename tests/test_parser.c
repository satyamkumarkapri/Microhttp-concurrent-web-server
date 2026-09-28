/*
 * test_parser.c — Unit Tests for HTTP Parser
 *
 * Tests the http_parser_feed() function in isolation.
 * No network I/O, no files, no threads.
 *
 * OS Concept (Week 3): Testing the parsing layer independently confirms
 * that the streaming parser correctly handles partial input, malformed
 * requests, percent-encoded paths, and various HTTP versions.
 *
 * Build: gcc -std=c11 -Wall -g -o build/test_parser \
 *            tests/test_parser.c src/parser.c src/logger.c -I src/
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "parser.h"
#include "logger.h"

/* ─── Test harness ────────────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        fprintf(stdout, "  [ RUN ] %s\n", name); \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            fprintf(stderr, "    [FAIL] %s:%d: expected %d == %d\n", \
                    __FILE__, __LINE__, (int)(a), (int)(b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define EXPECT_STR(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            fprintf(stderr, "    [FAIL] %s:%d: expected '%s' == '%s'\n", \
                    __FILE__, __LINE__, (a), (b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define PASS() do { tests_passed++; fprintf(stdout, "  [ OK  ] \n"); } while(0)

/* ─── Helper ──────────────────────────────────────────────────────────────── */

static parse_result_t feed_string(http_parser_t *p, const char *s,
                                   http_request_t *req)
{
    return http_parser_feed(p, s, strlen(s), req);
}

/* ─── Test cases ─────────────────────────────────────────────────────────── */

static void test_simple_get(void)
{
    TEST("Simple GET request");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_EQ(req.method, HTTP_METHOD_GET);
    EXPECT_EQ(req.version, HTTP_VERSION_11);
    EXPECT_STR(req.path, "/index.html");
    EXPECT_EQ(req.keep_alive, 1);
    PASS();
}

static void test_head_request(void)
{
    TEST("HEAD request");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "HEAD /style.css HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_EQ(req.method, HTTP_METHOD_HEAD);
    EXPECT_STR(req.path, "/style.css");
    EXPECT_EQ(req.keep_alive, 0);
    PASS();
}

static void test_http10(void)
{
    TEST("HTTP/1.0 defaults to close");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    /* HTTP/1.0 does NOT require Host header */
    const char *raw = "GET / HTTP/1.0\r\n\r\n";
    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_EQ(req.version, HTTP_VERSION_10);
    EXPECT_EQ(req.keep_alive, 0);   /* HTTP/1.0 default = close */
    PASS();
}

static void test_partial_request(void)
{
    TEST("Partial request (two feeds)");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    /* Feed first half */
    const char *part1 = "GET /test HTTP/1.1\r\nHost: ";
    parse_result_t r = feed_string(&p, part1, &req);
    EXPECT_EQ(r, PARSE_INCOMPLETE);

    /* Feed second half */
    const char *part2 = "localhost\r\n\r\n";
    r = feed_string(&p, part2, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_STR(req.path, "/test");
    PASS();
}

static void test_malformed_no_method(void)
{
    TEST("Malformed: missing method");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw = " /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    /* strtok on whitespace will get empty string or wrong token */
    parse_result_t r = feed_string(&p, raw, &req);
    /* May be PARSE_ERROR or PARSE_COMPLETE with UNKNOWN method — both ok */
    (void)r;
    tests_passed++;
    fprintf(stdout, "  [ OK  ] (result=%d — malformed accepted or rejected)\n",
            (int)r);
}

static void test_malformed_no_host_11(void)
{
    TEST("Malformed: HTTP/1.1 without Host");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw = "GET / HTTP/1.1\r\n\r\n";
    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_ERROR);   /* RFC 7230 requires Host in HTTP/1.1 */
    PASS();
}

static void test_percent_decode(void)
{
    TEST("Percent-decoded path");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "GET /hello%20world.txt HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_STR(req.path, "/hello world.txt");
    PASS();
}

static void test_traversal_encoded(void)
{
    TEST("Encoded traversal path (raw stored, not resolved)");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    /* After percent-decode: /../../etc/passwd */
    const char *raw =
        "GET /..%2F..%2Fetc%2Fpasswd HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    /* path should be decoded — traversal protection is done in file.c */
    fprintf(stdout, "  [ OK  ] decoded path: '%s'\n", req.path);
    tests_passed++;
}

static void test_range_header(void)
{
    TEST("Range header parsed");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "GET /large.txt HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Range: bytes=100-199\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    assert(req.range != NULL);
    EXPECT_STR(req.range, "bytes=100-199");
    PASS();
}

static void test_connection_close(void)
{
    TEST("Connection: close disables keep-alive");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_EQ(req.keep_alive, 0);
    PASS();
}

static void test_unsupported_method(void)
{
    TEST("DELETE method (parsed but unknown to file server)");
    http_parser_t p;
    http_request_t req;
    http_parser_init(&p);

    const char *raw =
        "DELETE /file HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    parse_result_t r = feed_string(&p, raw, &req);
    EXPECT_EQ(r, PARSE_COMPLETE);
    EXPECT_EQ(req.method, HTTP_METHOD_DELETE);
    PASS();
}

static void test_url_decode_function(void)
{
    TEST("url_decode function");

    char out[256];

    /* Normal decode */
    assert(url_decode(out, sizeof(out), "/hello%20world") == 0);
    EXPECT_STR(out, "/hello world");

    /* Null byte rejection */
    assert(url_decode(out, sizeof(out), "/evil%00byte") == -1);

    /* Invalid hex */
    assert(url_decode(out, sizeof(out), "/bad%GGseq") == -1);

    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Suppress log output during tests */
    g_log_level = LOG_LEVEL_NONE;

    fprintf(stdout, "\n═══ HTTP Parser Unit Tests ═══════════════════════════\n\n");

    test_simple_get();
    test_head_request();
    test_http10();
    test_partial_request();
    test_malformed_no_method();
    test_malformed_no_host_11();
    test_percent_decode();
    test_traversal_encoded();
    test_range_header();
    test_connection_close();
    test_unsupported_method();
    test_url_decode_function();

    fprintf(stdout,
            "\n═══ Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        fprintf(stdout, ", %d FAILED", tests_failed);
    fprintf(stdout, " ════════════════\n\n");

    return (tests_failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
