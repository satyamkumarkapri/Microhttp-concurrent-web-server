# MICROHTTP — Concurrent HTTP/1.1 Web Server

> **OS & Systems Programming — B.Tech CSE**
> A production-style academic project demonstrating Linux systems programming, POSIX threads, epoll, TCP sockets, and HTTP/1.1 implementation in pure C11.

---

## Overview

MICROHTTP is a concurrent HTTP/1.1 static web server built entirely from scratch using Linux system calls. It implements three distinct concurrency architectures so you can directly compare their performance and tradeoffs.

**No external HTTP libraries. No web frameworks. Just C11, sockets, and Linux syscalls.**

---

## Features

| Category | Feature |
|----------|---------|
| **Protocol** | HTTP/1.1 (GET, HEAD), HTTP/1.0 |
| **Concurrency** | Single-threaded, Thread Pool, epoll event loop |
| **Keep-Alive** | Persistent connections, configurable idle timeout |
| **File Serving** | Static files, MIME detection, `sendfile()` zero-copy |
| **Range Requests** | `Range: bytes=N-M` → 206 Partial Content |
| **Conditional GET** | `If-Modified-Since` → 304 Not Modified |
| **Security** | `realpath()` path traversal protection, encoded traversal rejection |
| **Signals** | `signalfd` (Linux) / self-pipe graceful shutdown |
| **Timers** | `timerfd` (Linux) idle connection timeout |
| **Status Codes** | 200, 206, 304, 400, 403, 404, 405, 408, 413, 416, 500, 501, 505 |
| **Testing** | Unit tests (C), integration tests (bash/curl), benchmarks |

---

## Architecture

### Three Concurrency Models

```
MODE 1: Single-Threaded (Baseline)
────────────────────────────────────
accept() → recv() → parse → respond → repeat
One client at a time. Establishes a performance baseline.

MODE 2: Thread Pool
────────────────────────────────────────────────────────
main thread                 worker threads (N)
──────────                  ─────────────────
accept()                    dequeue(job)
  │                           │
  ▼                           ▼
bounded job queue          recv/parse/respond
(pthread_mutex)               │
  │                           ▼
  └──── pthread_cond ──── loop back

MODE 3: epoll Event Loop (Linux Only)
──────────────────────────────────────────────────────────
Single thread manages all connections via epoll:

epoll_wait()
  ├── listen_fd    → accept4() → register client fd
  ├── client_fd₁  → recv/parse/respond (non-blocking)
  ├── client_fd₂  → recv/parse/respond (non-blocking)
  ├── timerfd     → scan for idle connections
  └── signalfd    → SIGINT/SIGTERM graceful shutdown
```

### Connection State Machine

```
CONN_READING
    │ (full request received)
    ▼
CONN_PROCESSING
    │ (response built)
    ▼
CONN_WRITING
    ├── (keep-alive) ──► CONN_KEEP_ALIVE ──► CONN_READING
    ├── (Connection: close) ──► CONN_CLOSING ──► [close(fd)]
    └── (idle timeout) ──► CONN_TIMEOUT ──► 408 ──► CONN_CLOSING
```

---

## Project Structure

```
microhttp-concurrent-web-server/
├── include/                Header files
│   ├── server.h            TCP listener, mode dispatcher
│   ├── http.h              HTTP response builder
│   ├── parser.h            Streaming HTTP/1.1 request parser
│   ├── connection.h        Connection state machine
│   ├── file.h              Path resolution, sendfile(), Range
│   ├── thread_pool.h       Bounded thread pool
│   ├── epoll_server.h      Linux epoll event loop
│   ├── signal_handler.h    signalfd / self-pipe shutdown
│   ├── timer.h             timerfd idle timeout
│   ├── mime.h              MIME type detection
│   └── logger.h            Thread-safe logging
├── src/                    Source implementations
│   ├── main.c              CLI entry point, getopt_long parsing
│   ├── server.c            TCP listener, mode dispatcher
│   ├── http.c              HTTP response builder
│   ├── parser.c            Streaming HTTP/1.1 request parser
│   ├── connection.c        Connection state machine
│   ├── file.c              Path resolution, sendfile(), Range
│   ├── thread_pool.c       Bounded thread pool
│   ├── epoll_server.c      Linux epoll event loop
│   ├── signal_handler.c    signalfd / self-pipe shutdown
│   ├── timer.c             timerfd idle timeout
│   ├── mime.c              MIME type detection
│   └── logger.c            Thread-safe logging
├── tests/
│   ├── test_parser.c       HTTP parser unit tests
│   ├── test_path.c         Path security unit tests
│   ├── test_http.sh        curl-based integration tests
│   └── ...                 More shell integration tests
├── benchmark/
│   ├── benchmark.sh        Compare all three modes
│   └── results/
├── public/                 Demo website
│   ├── index.html
│   ├── style.css
│   ├── script.js
│   ├── test.txt
│   └── large-test-file.txt
├── docs/
│   ├── architecture.md     Detailed architecture documentation
│   ├── concurrency.md      Concurrency model comparison
│   ├── viva.md             Viva Q&A preparation
│   └── ...
├── Makefile
├── README.md
├── LICENSE
└── .gitignore
```

---

## Build Instructions

### Requirements

- **GCC** (≥ 9.0) with C11 support
- **GNU make**
- **Ubuntu 20.04+** or **macOS** (for development only)
- **Linux** required for epoll mode, signalfd, timerfd, sendfile

```bash
# Install on Ubuntu
sudo apt install build-essential gcc make

# Optional for testing
sudo apt install curl apache2-utils  # ab benchmark tool
sudo apt install valgrind
sudo apt install wrk  # or install from source
```

### Building

```bash
# Clone the repository
git clone https://github.com/your-username/microhttp-concurrent-web-server
cd microhttp-concurrent-web-server

# Debug build (default)
make

# Release build (optimized)
make release

# AddressSanitizer build (memory safety)
make asan

# Show all make targets
make help
```

**Expected output:**
```
  ✓ Built: build/microhttp
  Run: build/microhttp --help
```

---

## Running the Server

```bash
# Single-threaded baseline
./build/microhttp --mode single --port 8080 --root ./public

# Thread pool (8 workers)
./build/microhttp --mode threadpool --port 8080 --root ./public --workers 8

# epoll event loop (Linux only)
./build/microhttp --mode epoll --port 8080 --root ./public

# With all options
./build/microhttp \
  --mode threadpool \
  --port 8080 \
  --root ./public \
  --workers 4 \
  --queue-size 256 \
  --timeout 30 \
  --max-connections 2048
```

### CLI Reference

| Option | Default | Description |
|--------|---------|-------------|
| `--port PORT` | 8080 | TCP listening port |
| `--root PATH` | `./public` | Document root directory |
| `--mode MODE` | `threadpool` | `single` \| `threadpool` \| `epoll` |
| `--workers N` | 4 | Thread pool worker count |
| `--queue-size N` | 128 | Job queue depth (backpressure limit) |
| `--timeout SECONDS` | 10 | Idle connection timeout |
| `--max-connections N` | 1024 | epoll mode connection limit |
| `--quiet` | off | Suppress INFO logs |
| `--help` | | Show help |

---

## HTTP Support

### Supported Methods
- `GET` — Retrieve a resource
- `HEAD` — Retrieve headers only (no body)

### Headers Parsed
| Request Header | Effect |
|----------------|--------|
| `Host` | Required in HTTP/1.1 |
| `Connection` | `keep-alive` or `close` |
| `Range` | Byte-range requests |
| `If-Modified-Since` | Conditional GET |
| `Content-Length` | Request body size |

### Headers Sent
`Date`, `Content-Type`, `Content-Length`, `Last-Modified`, `Connection`, `Content-Range`

### Status Codes
`200`, `206`, `304`, `400`, `403`, `404`, `405`, `408`, `413`, `416`, `500`, `501`, `505`

---

## Security

- **Path traversal**: `realpath()` canonicalizes paths; prefix check verifies result is under doc root
- **Encoded traversal**: `%2F` (URL-encoded `/`) is decoded before path check, then blocked
- **Null byte injection**: `%00` in URLs is rejected by the URL decoder
- **Oversized requests**: Requests exceeding 16 KB receive `413 Content Too Large`
- **Malformed headers**: `400 Bad Request` returned; connection closed
- **File descriptor limits**: `O_CLOEXEC` prevents fd leaks across `exec()`

---

## Testing

### Unit Tests (C)
```bash
make test
```
Runs `test_parser` (12 tests) and `test_path` (12 tests) — no server needed.

### Integration Tests (Ubuntu)
```bash
# Start server
./build/microhttp --mode threadpool --port 8080 &

# Run integration tests
bash tests/test_http.sh

# Stop server
kill -SIGTERM %1
```

Tests cover: GET, HEAD, 404, 403, 400, Range, If-Modified-Since, keep-alive, traversal.

---

## Benchmarking

> **Run on Ubuntu Linux for accurate results.**

```bash
# Install benchmark tools
sudo apt install apache2-utils  # ab
# or
sudo apt install wrk

# Run all benchmarks (single vs threadpool vs epoll)
chmod +x benchmark/benchmark.sh
./benchmark/benchmark.sh 8080 30 100

# Results saved to benchmark/results/
```

The script tests each mode sequentially and reports:
- Requests per second
- Successful requests
- Error count
- Latency percentiles (wrk: p50, p99)

---

## Valgrind Memory Testing

> **Run on Ubuntu Linux** (Valgrind is not available on macOS Apple Silicon)

```bash
make valgrind
```

Or manually:
```bash
valgrind \
  --leak-check=full \
  --show-leak-kinds=all \
  --track-origins=yes \
  --error-exitcode=1 \
  ./build/microhttp --mode threadpool --port 8080 --root ./public

# In another terminal:
curl http://localhost:8080/index.html
curl http://localhost:8080/test.txt
curl -H "Range: bytes=0-9" http://localhost:8080/test.txt

# Send SIGTERM to trigger clean shutdown
kill -SIGTERM <pid>

# Check valgrind-report.txt
```

---

## Graceful Shutdown

Press `Ctrl+C` (SIGINT) or send SIGTERM:

```
[INFO] Received signal 2 — initiating graceful shutdown
[INFO] Graceful shutdown: draining thread pool...
[INFO] Thread pool shutting down...
[INFO] Thread pool shutdown complete
[INFO] Server exited cleanly (status=0)
```

The server:
1. Sets the global `g_shutdown_requested` atomic flag
2. Stops accepting new connections
3. Signals worker threads via condition broadcast
4. Joins all threads (waits for in-flight requests to complete)
5. Closes all file descriptors
6. Frees all heap memory
7. Returns exit code 0

---

## OS Concepts Demonstrated

| Week | Concept | Implementation |
|------|---------|----------------|
| 1 | Syscall boundary | `socket()`, `bind()`, `listen()`, `accept()` |
| 2 | Memory model | `malloc`/`free`, explicit conn ownership |
| 3 | Parsing/I/O | Streaming HTTP parser, partial `recv()` |
| 4 | Processes/threads | `pthread_create`, thread pool workers |
| 5 | File resolution | `stat()`, `open()`, `realpath()`, MIME |
| 6 | Signals/timers | `signalfd`, `timerfd`, `SIGINT`/`SIGTERM` |
| 7 | FDs/pipes | `dup2`, `O_CLOEXEC`, keep-alive, EOF |
| 8 | Virtual memory | No leaks, Valgrind clean |
| 9 | Files/inodes | `fstat`, `sendfile()`, Range, `Last-Modified` |
| 10 | Threads/mutex | Thread pool, `pthread_mutex_t`, `pthread_cond_t` |
| 11 | Semaphores/deadlock | Bounded queue, backpressure, clean shutdown |
| 12 | Integration | Full test suite, benchmark, documentation |

---

## Design Decisions

### Why `realpath()` for security?
`realpath()` resolves the path through the kernel's VFS, following every symlink and normalizing `..` components. It returns the true absolute path. No amount of URL encoding tricks can bypass this.

### Why bounded work queue?
An unbounded queue means a slow server under high load accumulates memory. The bounded queue provides backpressure — the accept loop waits when the queue is full, naturally throttling new connections.

### Why `signalfd` over `SA_HANDLER`?
Signal handlers run asynchronously and cannot safely call most library functions (no `malloc`, no `printf`). `signalfd` converts signals to file descriptor events, letting us handle them in the normal event loop with full library access.

### Why `sendfile()` for file transfer?
`sendfile(out_fd, in_fd, &offset, count)` copies data in the kernel without going through user space — zero copies. For large files this is significantly more efficient than `read()` + `write()`.

---

## Limitations

- Single-range requests only (multi-range `bytes=0-99,200-299` not implemented)
- No HTTPS/TLS (out of scope for OS project)
- No CGI or dynamic content
- epoll mode uses global fd table — max fd 65535
- No HTTP/2 support
- IPv4 only

---

## Future Improvements

- HTTP/2 with HPACK compression
- HTTPS via LibreSSL/OpenSSL
- Dynamic content via CGI subprocess
- Multi-range support
- IPv6 (`AF_INET6`)
- Sendfile fallback to `splice()`
- Access logging to file
- Virtual host support
- Connection rate limiting

---

## Team Contribution

| Member | Component |
|--------|-----------|
| Satyam | (       ) |
| Rishika | (       ) |

---

*Built with C11, Linux system calls, POSIX threads, and zero external HTTP libraries.*
