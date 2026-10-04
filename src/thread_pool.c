/*
 * thread_pool.c — Bounded Thread Pool Implementation
 *
 * OS Concepts (Week 4, 10, 11):
 *
 * pthread_create():     Creates an OS thread. Each thread has its own stack
 *                       but shares the process address space.
 *
 * pthread_mutex_t:      Binary mutex — only one thread holds it at a time.
 *                       Protects the queue head/tail/count from data races.
 *
 * pthread_cond_t:       Condition variable. Allows a thread to atomically
 *                       release the mutex and wait for a signal. Wakes up
 *                       when another thread calls pthread_cond_signal().
 *
 * Bounded queue:        Limits memory usage. When the queue is full, the
 *                       submitter waits on 'not_full' — this is backpressure.
 *                       When empty, workers wait on 'not_empty'.
 *
 * Graceful shutdown:    We set pool->shutdown = 1 and broadcast on both
 *                       condition variables, waking all waiting threads.
 *                       Workers check the flag and exit their loop.
 *
 * Deadlock avoidance:   We always acquire the mutex before touching the queue.
 *                       pthread_cond_wait() atomically releases the mutex
 *                       while sleeping, so no deadlock with the submitter.
 */


#define _POSIX_C_SOURCE 200809L

#include "thread_pool.h"
#include "connection.h"
#include "http.h"
#include "parser.h"
#include "logger.h"
#include "signal_handler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ─── Internal queue ring buffer ──────────────────────────────────────────── */

struct thread_pool {
    /* Queue (ring buffer) */
    tp_job_t       *queue;          /* heap-allocated circular buffer */
    size_t          capacity;       /* max jobs in the queue           */
    size_t          head;           /* index to dequeue from           */
    size_t          tail;           /* index to enqueue at             */
    size_t          count;          /* current number of jobs          */

    /* Workers */
    pthread_t      *workers;        /* array of worker thread IDs      */
    size_t          num_workers;

    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;      /* signalled when a job is added   */
    pthread_cond_t  not_full;       /* signalled when a job is removed */

    /* Shutdown flag */
    int             shutdown;

    /* Server config (passed to each worker) */
    char            doc_root[4096];
    int             idle_timeout;
};

/* ─── Worker body ─────────────────────────────────────────────────────────── */

/*
 * handle_connection_blocking — drives the HTTP keep-alive state machine
 * for one connection on a single worker thread.
 *
 * A worker thread handles one connection at a time: it reads all requests
 * on that connection (keep-alive loop) until the connection closes or times out.
 */
static void handle_connection_blocking(int client_fd, const char *doc_root,
                                       int idle_timeout_sec)
{
    /* Receive buffer for accumulating request bytes */
    char buf[4096];

    /* Per-connection HTTP parser */
    http_parser_t parser;
    http_parser_init(&parser);

    /* Set socket receive timeout (idle timeout) */
    struct timeval tv;
    tv.tv_sec  = idle_timeout_sec;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int keep_alive = 1;

    while (keep_alive && !atomic_load(&g_shutdown_requested)) {
        /* Reset parser for each new request in the keep-alive loop */
        http_parser_init(&parser);

        parse_result_t result = PARSE_INCOMPLETE;
        http_request_t req;

        /* ── Read loop: accumulate bytes until request is complete ── */
        while (result == PARSE_INCOMPLETE) {
            ssize_t nr = recv(client_fd, buf, sizeof(buf), 0);
            if (nr < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* SO_RCVTIMEO expired — idle timeout */
                    LOG_INFO("Client idle timeout on fd=%d", client_fd);
                    /* Send 408 Request Timeout */
                    http_send_error(client_fd, 408, 0);
                    return;
                }
                if (errno == EINTR) continue;
                LOG_DEBUG("recv error on fd=%d: %s", client_fd, strerror(errno));
                return;
            }
            if (nr == 0) {
                /* Client closed connection (TCP FIN) */
                LOG_DEBUG("Client disconnected (EOF) on fd=%d", client_fd);
                return;
            }

            result = http_parser_feed(&parser, buf, (size_t)nr, &req);

            if (result == PARSE_TOO_LARGE) {
                http_send_error(client_fd, 413, 0);
                return;
            }
            if (result == PARSE_ERROR) {
                http_send_error(client_fd, 400, 0);
                return;
            }
        }

        /* ── Request is fully parsed — handle it ── */
        keep_alive = http_handle_request(client_fd, doc_root, &req);
    }
}

/*
 * worker_thread — the body of each worker thread.
 *
 * Waits for jobs from the queue, handles them, loops back.
 */
static void *worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;

    for (;;) {
        /* ── Lock and wait for a job ── */
        pthread_mutex_lock(&pool->mutex);

        /*
         * pthread_cond_wait atomically:
         *   1. Releases the mutex.
         *   2. Puts the thread to sleep.
         * When signalled:
         *   3. Reacquires the mutex before returning.
         *
         * We loop on the condition to handle spurious wakeups.
         */
        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        /* Check shutdown with an empty queue */
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        /* Dequeue one job (ring buffer FIFO) */
        tp_job_t job = pool->queue[pool->head];
        pool->head   = (pool->head + 1) % pool->capacity;
        pool->count--;

        /* Signal the submitter that space is available */
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        /* ── Handle the connection (outside the lock) ── */
        LOG_DEBUG("Worker handling fd=%d peer=%s", job.client_fd, job.peer_addr);

        handle_connection_blocking(job.client_fd, pool->doc_root,
                                   pool->idle_timeout);
        close(job.client_fd);
        LOG_DEBUG("Worker done with fd=%d", job.client_fd);
    }

    return NULL;
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

thread_pool_t *thread_pool_create(const tp_config_t *cfg)
{
    if (!cfg || cfg->num_workers == 0 || cfg->queue_size == 0)
        return NULL;

    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (!pool) { LOG_SYSERR("calloc (thread_pool)"); return NULL; }

    pool->queue = calloc(cfg->queue_size, sizeof(tp_job_t));
    if (!pool->queue) {
        LOG_SYSERR("calloc (queue)");
        free(pool);
        return NULL;
    }

    pool->workers = calloc(cfg->num_workers, sizeof(pthread_t));
    if (!pool->workers) {
        LOG_SYSERR("calloc (workers)");
        free(pool->queue);
        free(pool);
        return NULL;
    }

    pool->capacity     = cfg->queue_size;
    pool->num_workers  = cfg->num_workers;
    pool->idle_timeout = cfg->idle_timeout;
    pool->shutdown     = 0;
    pool->head = pool->tail = pool->count = 0;

    snprintf(pool->doc_root, sizeof(pool->doc_root), "%s", cfg->doc_root);

    /* Initialize synchronization primitives */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        LOG_SYSERR("pthread_mutex_init");
        goto fail;
    }
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        LOG_SYSERR("pthread_cond_init (not_empty)");
        pthread_mutex_destroy(&pool->mutex);
        goto fail;
    }
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        LOG_SYSERR("pthread_cond_init (not_full)");
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->mutex);
        goto fail;
    }

    /* Spawn worker threads */
    for (size_t i = 0; i < pool->num_workers; i++) {
        int r = pthread_create(&pool->workers[i], NULL, worker_thread, pool);
        if (r != 0) {
            LOG_ERROR("pthread_create failed for worker %zu: %s",
                      i, strerror(r));
            pool->num_workers = i;  /* only join threads that started */
            thread_pool_shutdown(pool);
            return NULL;
        }
        LOG_DEBUG("Worker thread %zu started", i);
    }

    LOG_INFO("Thread pool started: %zu workers, queue size %zu",
             pool->num_workers, pool->capacity);
    return pool;

fail:
    free(pool->workers);
    free(pool->queue);
    free(pool);
    return NULL;
}

int thread_pool_submit(thread_pool_t *pool, const tp_job_t *job)
{
    pthread_mutex_lock(&pool->mutex);

    /* Block while queue is full (backpressure) */
    while (pool->count >= pool->capacity && !pool->shutdown) {
        LOG_DEBUG("Queue full (%zu/%zu) — submitter waiting",
                  pool->count, pool->capacity);
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    /* Enqueue (ring buffer) */
    pool->queue[pool->tail] = *job;
    pool->tail  = (pool->tail + 1) % pool->capacity;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void thread_pool_shutdown(thread_pool_t *pool)
{
    if (!pool) return;

    LOG_INFO("Thread pool shutting down...");

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    /* Wake all waiting threads */
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->mutex);

    /* Join all worker threads */
    for (size_t i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i], NULL);
        LOG_DEBUG("Worker thread %zu joined", i);
    }

    /* Destroy remaining jobs (close fds) */
    pthread_mutex_lock(&pool->mutex);
    while (pool->count > 0) {
        tp_job_t *job = &pool->queue[pool->head];
        close(job->client_fd);
        pool->head  = (pool->head + 1) % pool->capacity;
        pool->count--;
    }
    pthread_mutex_unlock(&pool->mutex);

    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);

    free(pool->workers);
    free(pool->queue);
    free(pool);

    LOG_INFO("Thread pool shutdown complete");
}
