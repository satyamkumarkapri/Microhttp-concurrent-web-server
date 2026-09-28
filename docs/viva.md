# MICROHTTP — Viva Preparation Q&A

> **OS & Systems Programming — Final Viva**
> All answers are grounded in the actual MICROHTTP implementation.

---

## Part 1: Socket Fundamentals

### What is a socket?

A socket is a kernel-managed communication endpoint. When your program calls `socket(AF_INET, SOCK_STREAM, 0)`, the kernel allocates an internal data structure (a `struct socket`) and returns a **file descriptor** — an integer index into the process's open-file table — that refers to it. A socket is the kernel's abstraction for a bidirectional byte stream between two endpoints.

In MICROHTTP: `server_create_listener()` in `server.c` creates the listening socket.

### What does bind() do?

`bind(fd, addr, addrlen)` associates a socket with a specific local IP address and port number. The kernel writes this binding into its internal socket table. From this point on, incoming TCP segments destined for `addr:port` are routed to this socket.

Without `bind()`, the OS assigns an ephemeral (random) port — fine for clients, not servers.

### What does listen() do?

`listen(fd, backlog)` transitions a socket from `CLOSED` state to `LISTEN` state. The `backlog` parameter sets the maximum number of fully-established TCP connections that have completed the 3-way handshake but have not yet been `accept()`-ed by the application. If this queue fills up, the kernel silently drops new SYN packets.

In MICROHTTP: `LISTEN_BACKLOG = 128` (set in `server.h`).

### What does accept() do?

`accept(listen_fd, addr, addrlen)` dequeues one connection from the kernel's accept queue. It **blocks** if the queue is empty (no clients waiting). It returns a **new file descriptor** representing the established TCP connection. The original `listen_fd` remains open and can accept more connections.

This is a critical point: `listen_fd` ≠ `client_fd`. The listen fd is the "door," the client fd is the "room."

### What is a file descriptor?

A file descriptor is a non-negative integer that the kernel uses as an index into the process's open-file table. It can refer to a regular file, a socket, a pipe, a terminal, an epoll instance, or a timerfd. Everything in Linux is a file.

Every process starts with fd 0 (stdin), 1 (stdout), 2 (stderr).

`O_CLOEXEC` (used throughout MICROHTTP) ensures fds are automatically closed when `exec()` is called.

---

## Part 2: Concurrency

### What is epoll?

`epoll` is a Linux kernel interface for scalable I/O event notification. It maintains a kernel-side red-black tree of watched file descriptors. When a watched fd becomes ready (readable, writable, or has an error), the kernel moves it to a ready list. `epoll_wait()` returns only the ready events — it does **not** scan all registered fds.

Complexity: `epoll_wait()` is O(number of ready events), whereas `select()`/`poll()` is O(number of registered fds).

MICROHTTP epoll mode: `epoll_server.c` — registers the listen fd, all client fds, a timerfd, and a signalfd on the same epoll instance.

### Why use nonblocking sockets?

In an event loop, a single thread manages many connections. If a `recv()` on one slow client were to block, every other client would be starved. With `O_NONBLOCK`, `recv()` returns immediately with `EAGAIN` if no data is available. The thread then moves to the next ready fd and comes back when epoll reports data.

MICROHTTP: `set_nonblocking()` in `epoll_server.c` uses `fcntl(fd, F_SETFL, O_NONBLOCK)`.

### What is a thread pool?

A thread pool pre-creates N OS threads. A shared work queue holds pending jobs. Worker threads sleep on a condition variable when the queue is empty. The main thread accepts connections and submits them to the queue. This avoids the overhead of creating/destroying OS threads per request.

MICROHTTP: `thread_pool.c`. Workers call `pthread_cond_wait(&pool->not_empty, &pool->mutex)`.

### Why use mutex?

A mutex (mutual exclusion lock) ensures that only one thread at a time can read or modify shared state. Without it, two threads could simultaneously modify the queue's `head`/`tail`/`count` fields, causing a **data race** — undefined behavior.

MICROHTTP: `pthread_mutex_lock(&pool->mutex)` before any queue access.

### Why use condition variables?

A mutex alone would require a thread to **spin** (busy-wait) checking if the queue is non-empty. A condition variable (`pthread_cond_t`) allows a thread to **sleep** while atomically releasing the mutex. When another thread signals the condition (via `pthread_cond_signal()`), the sleeping thread wakes and reacquires the mutex.

`pthread_cond_wait()` atomically:
1. Releases the mutex.
2. Puts the thread to sleep.
3. On signal: reacquires the mutex before returning.

### What is a race condition?

A race condition occurs when two or more threads access shared mutable state concurrently without synchronization, and the final outcome depends on the scheduling order (which is non-deterministic). Race conditions cause intermittent, hard-to-reproduce bugs.

Example: Two threads both read `count = 5`, both add 1, both write `count = 6`. Correct result should be 7.

MICROHTTP prevents this with `pthread_mutex_t` protecting the job queue.

### What is deadlock?

Deadlock occurs when two or more threads each hold a resource the other needs, causing permanent blocking. The four Coffman conditions: mutual exclusion, hold and wait, no preemption, circular wait.

MICROHTTP avoids deadlock by:
- Having only one mutex (no circular wait possible).
- Always releasing the mutex in the correct order.
- Broadcasting on shutdown so no thread can get stuck.

---

## Part 3: HTTP Protocol

### What is keep-alive?

HTTP keep-alive (persistent connections) allows multiple HTTP request/response pairs over a single TCP connection. In HTTP/1.1, connections are persistent by default. The server closes the connection only when it sees `Connection: close` or an idle timeout.

MICROHTTP: After `http_handle_request()` returns, if `keep_alive == 1`, the worker loops back and calls `http_parser_init()` for the next request.

### What is a partial read?

`recv()` may return fewer bytes than requested. This is because TCP delivers whatever bytes are currently in the kernel receive buffer, which may be less than the full HTTP request. An HTTP server must accumulate bytes across multiple `recv()` calls until the full request (terminated by `\r\n\r\n`) arrives.

MICROHTTP: `http_parser_feed()` in `parser.c` is called with each chunk. It returns `PARSE_INCOMPLETE` until the full request is buffered.

### What is a partial write?

`write()`/`send()` to a socket may transfer fewer bytes than requested if the kernel send buffer is full. The application must loop and retry with the remaining bytes.

MICROHTTP: `write_all()` in `http.c` loops until all bytes are sent.

### Why can recv() return fewer bytes?

TCP is a **stream protocol**, not a message protocol. The kernel delivers bytes from the receive buffer as they arrive. Network segmentation, MTU limits, Nagle's algorithm, and kernel scheduling all affect how bytes are grouped. Never assume one `send()` corresponds to one `recv()`.

---

## Part 4: Signals and Timers

### What is graceful shutdown?

On `SIGINT`/`SIGTERM`, instead of immediately calling `exit()`, the server:
1. Sets `g_shutdown_requested = 1`.
2. Stops accepting new connections.
3. Lets in-flight requests complete.
4. Signals worker threads to stop.
5. Joins all threads.
6. Closes fds, frees memory, destroys mutexes.
7. Exits cleanly.

### Why is signal handling difficult?

Signal handlers are invoked **asynchronously** — the thread can be interrupted anywhere, including inside library functions like `malloc()` or `printf()`. Most C library functions are **not** async-signal-safe (they use internal locks that may be held). Calling them from a signal handler can deadlock.

POSIX defines a limited set of async-signal-safe functions (e.g., `write(2)`, `_exit(2)`).

### What is signalfd?

`signalfd(2)` is a Linux system call that creates a file descriptor that becomes readable when a specified signal is received. Combined with blocking signals via `sigprocmask()`, this "converts" signals into normal I/O events. The epoll event loop can then handle `SIGINT`/`SIGTERM` without any signal handler — just a `read()` on the signalfd.

MICROHTTP: `signal_handler.c` — on Linux uses `signalfd()`, on macOS uses the self-pipe trick.

### What is timerfd?

`timerfd_create(CLOCK_MONOTONIC, ...)` creates a file descriptor that becomes readable when a timer expires. `timerfd_settime()` arms it. Reading the fd returns a `uint64_t` count of how many times the timer has fired.

This allows idle timeout scans to be driven by the epoll event loop — no separate timer thread required.

MICROHTTP: `timer.c` — creates a repeating timerfd. `epoll_server.c` adds it to epoll and calls `scan_idle()` on each expiry.

---

## Part 5: File System

### What is sendfile?

`sendfile(out_fd, in_fd, &offset, count)` is a Linux kernel call that transfers data from a file fd to a socket fd **entirely within the kernel**, without copying to user space. This is called "zero-copy" because the data goes directly from the page cache to the socket buffer.

Without sendfile: `read()` copies kernel page cache → user buffer; `write()` copies user buffer → socket buffer. Two copies. With sendfile: zero copies.

MICROHTTP: `file_serve_range()` in `file.c` — uses `sendfile()` on Linux, falls back to `read()`/`write()` loop on macOS.

### How does path traversal happen?

A client sends a URL like `/../../etc/passwd`. If the server naively concatenates `doc_root + url_path` and opens the result, it resolves to `/etc/passwd` — outside the document root.

### How is traversal prevented?

1. Percent-decode the URL path (our `url_decode()` in `parser.c`).
2. Concatenate `doc_root + decoded_path` to form a candidate.
3. Call `realpath()` which resolves all `..` and symlinks via the VFS.
4. Verify the canonical result starts with `doc_root`. If not → 403.

MICROHTTP: `file_resolve_path()` in `file.c` — the security chokepoint.

---

## Part 6: HTTP Status Codes

| Code | When issued |
|------|-------------|
| 200  | File found and served normally |
| 206  | `Range:` header present and satisfiable |
| 304  | `If-Modified-Since` and file not changed |
| 400  | Malformed request line/headers |
| 403  | Path traversal detected, or non-regular file |
| 404  | File not found under doc root |
| 405  | Method is not GET or HEAD |
| 408  | Idle timeout expired on client socket |
| 413  | Request headers exceeded `HTTP_MAX_REQUEST_SIZE` (16 KB) |
| 416  | Range not satisfiable (start > file size) |
| 500  | Internal error (file open failed after resolve) |
| 501  | Completely unknown HTTP method |
| 505  | HTTP version is not 1.0 or 1.1 |

---

## Part 7: Range Requests and Conditional GET

### How does Range work?

The client sends: `Range: bytes=0-99`

The server:
1. Parses the range in `file_parse_range()`.
2. Validates it against the file size.
3. Responds with `206 Partial Content`.
4. Includes `Content-Range: bytes 0-99/1024` and `Content-Length: 100`.
5. Uses `lseek()` + `sendfile()` to send exactly those bytes.

### How does If-Modified-Since work?

The client sends: `If-Modified-Since: Mon, 01 Jan 2024 00:00:00 GMT`

The server:
1. Parses the date with `strptime()`.
2. Compares it to the file's `st_mtime` (from `stat()`).
3. If `mtime <= client_time` → `304 Not Modified` (no body sent).
4. Otherwise → `200 OK` with full body.

This allows browsers/proxies to cache resources efficiently.

---

## Part 8: Architecture Comparison

### Thread pool vs epoll — when to use which?

| Aspect | Thread Pool | epoll Event Loop |
|--------|-------------|-----------------|
| Concurrency | Real parallelism on multi-core | Single-threaded, I/O-bound only |
| Memory | N × (thread stack ~8MB + overhead) | Per-connection state only |
| Complexity | Mutex/condvar synchronization | State machine per connection |
| CPU-bound work | Yes — workers run in parallel | No — blocks the event loop |
| Context switches | Yes — OS schedules N threads | Minimal — single thread |
| Best for | Mixed CPU/IO workloads | I/O-bound, many idle connections |

### What happens when 1000 clients connect?

- **Single mode**: 999 clients wait in the accept queue. Latency = sum of all previous request times.
- **Thread pool (8 workers)**: ~8 requests served concurrently. Queue depth up to 128. Clients beyond that wait in accept queue.
- **epoll mode**: All 1000 connections are registered in the epoll instance. The event loop handles whichever fds are ready. No client blocks the others (assuming I/O-bound).

### How is backpressure handled?

- **Thread pool**: `thread_pool_submit()` blocks on `pthread_cond_wait(&pool->not_full)` when the queue is full. The main accept loop is paused — new connections wait in the kernel accept queue.
- **epoll mode**: `max_connections` limit causes new connections to be `close()`d immediately with no response.

---

## Part 9: Memory Safety

### How did you check memory leaks?

```bash
valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes ./build/microhttp \
         --mode threadpool --port 8080 --root ./public
```

Key design decisions to achieve zero leaks:
- Every `conn_create()` has exactly one `conn_destroy()`.
- Thread pool shutdown closes all queued fds before `free()`.
- No `strdup()` without a matching `free()`.
- Parser uses a fixed-size buffer — no heap allocation during parsing.

### How did you benchmark the server?

```bash
./benchmark/benchmark.sh 8080 10 50
```

On Ubuntu with `wrk`:
```
wrk -t4 -c50 -d10s --latency http://localhost:8080/index.html
```

Compare single vs threadpool vs epoll mode. Results stored in `benchmark/results/`.

---

## Quick Reference: Key System Calls

| Call | Purpose | MICROHTTP location |
|------|---------|-------------------|
| `socket()` | Create fd | `server.c` |
| `bind()` | Assign address | `server.c` |
| `listen()` | Accept queue | `server.c` |
| `accept()` | Dequeue connection | `server.c` |
| `recv()` | Read from socket | `thread_pool.c`, `server.c` |
| `write()` | Write to socket | `http.c` |
| `sendfile()` | Zero-copy file→socket | `file.c` |
| `stat()` | File metadata | `file.c` |
| `realpath()` | Canonical path | `file.c` |
| `epoll_create1()` | Create epoll fd | `epoll_server.c` |
| `epoll_ctl()` | Register fd | `epoll_server.c` |
| `epoll_wait()` | Wait for events | `epoll_server.c` |
| `timerfd_create()` | Create timer | `timer.c` |
| `signalfd()` | Signal→fd | `signal_handler.c` |
| `pthread_create()` | New thread | `thread_pool.c` |
| `pthread_mutex_lock()` | Acquire mutex | `thread_pool.c` |
| `pthread_cond_wait()` | Sleep on condition | `thread_pool.c` |
| `close()` | Release fd | everywhere |
