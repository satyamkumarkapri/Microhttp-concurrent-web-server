/*
 * main.c — MICROHTTP Entry Point and CLI
 *
 * Parses command-line arguments and starts the appropriate server mode.
 *
 * OS Concept (Week 1):
 *   main(argc, argv) is the entry point defined by the C runtime (crt0/crt1).
 *   The OS loader (exec) sets up the initial stack with argc/argv pointers.
 *   We use getopt_long() (POSIX) to parse long-form options.
 *
 * Usage:
 *   ./microhttp [OPTIONS]
 *
 * Options:
 *   --port PORT          TCP port to listen on (default: 8080)
 *   --root PATH          Document root directory (default: ./public)
 *   --mode MODE          single | threadpool | epoll (default: threadpool)
 *   --workers N          Thread pool workers (default: 4)
 *   --queue-size N       Thread pool job queue depth (default: 128)
 *   --timeout SECONDS    Idle connection timeout (default: 10)
 *   --max-connections N  Max simultaneous connections, epoll mode (default: 1024)
 *   --quiet              Suppress INFO-level log output
 *   --help               Show this help
 */


#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "logger.h"
#include "signal_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>

/* ─── Banner ──────────────────────────────────────────────────────────────── */

static void print_banner(void)
{
    fprintf(stderr,
        "\n"
        "  ███╗   ███╗██╗ ██████╗██████╗  ██████╗ ██╗  ██╗████████╗████████╗██████╗\n"
        "  ████╗ ████║██║██╔════╝██╔══██╗██╔═══██╗██║  ██║╚══██╔══╝╚══██╔══╝██╔══██╗\n"
        "  ██╔████╔██║██║██║     ██████╔╝██║   ██║███████║   ██║      ██║   ██████╔╝\n"
        "  ██║╚██╔╝██║██║██║     ██╔══██╗██║   ██║██╔══██║   ██║      ██║   ██╔═══╝\n"
        "  ██║ ╚═╝ ██║██║╚██████╗██║  ██║╚██████╔╝██║  ██║   ██║      ██║   ██║\n"
        "  ╚═╝     ╚═╝╚═╝ ╚═════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚═╝\n"
        "\n"
        "  Concurrent HTTP/1.1 Web Server — OS & Systems Programming Project\n"
        "\n"
    );
}

/* ─── Help ────────────────────────────────────────────────────────────────── */

static void print_help(const char *prog)
{
    fprintf(stdout,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "MICROHTTP — A Concurrent HTTP/1.1 Web Server\n"
        "\n"
        "Options:\n"
        "  --port PORT            TCP port to listen on         (default: %d)\n"
        "  --root PATH            Document root directory        (default: %s)\n"
        "  --mode MODE            Server mode:                   (default: threadpool)\n"
        "                           single      — single-threaded baseline\n"
        "                           threadpool  — thread pool with N workers\n"
        "                           epoll       — Linux epoll event loop [Linux only]\n"
        "  --workers N            Thread pool worker count       (default: %d)\n"
        "  --queue-size N         Thread pool job queue depth    (default: %d)\n"
        "  --timeout SECONDS      Idle connection timeout        (default: %d)\n"
        "  --max-connections N    epoll mode connection limit    (default: %d)\n"
        "  --quiet                Suppress INFO logs (WARN/ERROR still shown)\n"
        "  --help                 Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s --mode single --port 8080 --root ./public\n"
        "  %s --mode threadpool --port 8080 --root ./public --workers 8\n"
        "  %s --mode epoll --port 8080 --root ./public\n"
        "\n"
        "Notes:\n"
        "  - epoll mode requires Linux. Use Ubuntu for final testing.\n"
        "  - Press Ctrl+C (SIGINT) for graceful shutdown.\n"
        "\n",
        prog,
        DEFAULT_PORT, DEFAULT_DOC_ROOT,
        DEFAULT_WORKERS, DEFAULT_QUEUE_SIZE,
        DEFAULT_IDLE_TIMEOUT, DEFAULT_MAX_CONNECTIONS,
        prog, prog, prog
    );
}

/* ─── CLI Parsing ─────────────────────────────────────────────────────────── */

static int parse_int_arg(const char *name, const char *val,
                          int min_val, int max_val)
{
    char *end;
    long n = strtol(val, &end, 10);
    if (*end != '\0' || n < min_val || n > max_val) {
        fprintf(stderr, "[ERROR] Invalid value for --%s: '%s' "
                        "(must be %d..%d)\n", name, val, min_val, max_val);
        exit(EXIT_FAILURE);
    }
    return (int)n;
}

int main(int argc, char *argv[])
{
    server_config_t cfg = {
        .mode            = SERVER_MODE_THREADPOOL,
        .port            = DEFAULT_PORT,
        .doc_root        = DEFAULT_DOC_ROOT,
        .num_workers     = DEFAULT_WORKERS,
        .queue_size      = DEFAULT_QUEUE_SIZE,
        .idle_timeout    = DEFAULT_IDLE_TIMEOUT,
        .max_connections = DEFAULT_MAX_CONNECTIONS,
        .quiet           = 0,
    };

    static const struct option long_opts[] = {
        { "port",            required_argument, NULL, 'p' },
        { "root",            required_argument, NULL, 'r' },
        { "mode",            required_argument, NULL, 'm' },
        { "workers",         required_argument, NULL, 'w' },
        { "queue-size",      required_argument, NULL, 'q' },
        { "timeout",         required_argument, NULL, 't' },
        { "max-connections", required_argument, NULL, 'c' },
        { "quiet",           no_argument,       NULL, 'Q' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL,              0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:r:m:w:q:t:c:Qh",
                               long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                cfg.port = parse_int_arg("port", optarg, 1, 65535);
                break;
            case 'r':
                cfg.doc_root = optarg;
                break;
            case 'm':
                if (strcmp(optarg, "single") == 0)
                    cfg.mode = SERVER_MODE_SINGLE;
                else if (strcmp(optarg, "threadpool") == 0)
                    cfg.mode = SERVER_MODE_THREADPOOL;
                else if (strcmp(optarg, "epoll") == 0)
                    cfg.mode = SERVER_MODE_EPOLL;
                else {
                    fprintf(stderr,
                        "[ERROR] Unknown mode '%s'. Use: single, threadpool, epoll\n",
                        optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'w':
                cfg.num_workers = parse_int_arg("workers", optarg, 1, 1024);
                break;
            case 'q':
                cfg.queue_size = parse_int_arg("queue-size", optarg, 1, 65536);
                break;
            case 't':
                cfg.idle_timeout = parse_int_arg("timeout", optarg, 1, 3600);
                break;
            case 'c':
                cfg.max_connections = parse_int_arg(
                    "max-connections", optarg, 1, 65535);
                break;
            case 'Q':
                cfg.quiet = 1;
                break;
            case 'h':
                print_help(argv[0]);
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* Apply quiet mode */
    if (cfg.quiet)
        g_log_level = LOG_LEVEL_WARN;

    print_banner();

    /* ── Startup logging ── */
    const char *mode_str = (cfg.mode == SERVER_MODE_SINGLE)     ? "single"     :
                           (cfg.mode == SERVER_MODE_THREADPOOL)  ? "threadpool" : "epoll";

    LOG_INFO("Starting MICROHTTP");
    LOG_INFO("Mode:            %s", mode_str);
    LOG_INFO("Port:            %d", cfg.port);
    LOG_INFO("Document root:   %s", cfg.doc_root);
    LOG_INFO("Idle timeout:    %ds", cfg.idle_timeout);

    if (cfg.mode == SERVER_MODE_THREADPOOL) {
        LOG_INFO("Workers:         %d", cfg.num_workers);
        LOG_INFO("Queue size:      %d", cfg.queue_size);
    }
    if (cfg.mode == SERVER_MODE_EPOLL) {
        LOG_INFO("Max connections: %d", cfg.max_connections);
    }

    /* ── Create listening socket ── */
    int listen_fd = server_create_listener(cfg.port);
    if (listen_fd < 0) {
        fprintf(stderr, "[FATAL] Failed to create listening socket on port %d\n",
                cfg.port);
        return EXIT_FAILURE;
    }

    /* ── Run the server ── */
    int ret = server_run(listen_fd, &cfg);

    /* ── Cleanup ── */
    close(listen_fd);
    LOG_INFO("Server exited cleanly (status=%d)", ret);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
