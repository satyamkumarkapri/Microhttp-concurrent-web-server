/*
 * thread_pool.h — Bounded Thread Pool Interface
 *
 * OS Concept (Week 4, 10, 11):
 *   A thread pool pre-creates N worker threads. Client connections are
 *   placed into a bounded work queue. Workers dequeue connections and
 *   handle them. This avoids the overhead of creating a new OS thread for
 *   every connection.
 *
 *   Synchronization uses:
 *   - pthread_mutex_t: mutual exclusion for the queue (one thread at a time).
 *   - pthread_cond_t (not_empty): workers wait here when the queue is empty.
 *   - pthread_cond_t (not_full): the acceptor waits here when the queue is full.
 *
 *   The bounded queue provides backpressure: when the queue is full,
 *   the accept() thread waits rather than creating unlimited memory usage.
 *
 * Architecture:
 *
 *   main thread                  worker thread 1
 *   ─────────────                ────────────────
 *   accept(listen_fd)            dequeue(queue) → conn
 *     → conn_create()            http_handle_request(conn)
 *   thread_pool_submit(conn)     conn_destroy(conn)
 *                                wait for next job
 *
 *   main thread blocks on pthread_cond_wait(&not_full) when queue is full.
 *   Workers block on pthread_cond_wait(&not_empty) when queue is empty.
 */

#ifndef MICROHTTP_THREAD_POOL_H
#define MICROHTTP_THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

/* ─── Job type ────────────────────────────────────────────────────────────── */
typedef struct {
    int         client_fd;    /* accepted socket */
    /* addr is resolved before submitting, stored as string in connection_t */
    char        peer_addr[48];
} tp_job_t;

/* ─── Thread pool ─────────────────────────────────────────────────────────── */
typedef struct thread_pool thread_pool_t;

/* ─── Configuration ───────────────────────────────────────────────────────── */
typedef struct {
    size_t      num_workers;   /* number of worker threads */
    size_t      queue_size;    /* maximum pending jobs in queue */
    const char *doc_root;      /* document root to pass to each worker */
    int         idle_timeout;  /* connection idle timeout in seconds */
} tp_config_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/*
 * thread_pool_create — create and start the thread pool.
 * Returns a heap-allocated pool, or NULL on error.
 */
thread_pool_t *thread_pool_create(const tp_config_t *cfg);

/*
 * thread_pool_submit — submit a job (accepted connection) to the pool.
 *
 * Blocks if the queue is full (backpressure).
 * Returns 0 on success, -1 if the pool is shutting down.
 */
int thread_pool_submit(thread_pool_t *pool, const tp_job_t *job);

/*
 * thread_pool_shutdown — signal all workers to finish and join them.
 * After this call, the pool is destroyed and must not be used.
 */
void thread_pool_shutdown(thread_pool_t *pool);

#endif /* MICROHTTP_THREAD_POOL_H */
