/*
 * signal_handler.c — Graceful Shutdown via signalfd (Linux) / self-pipe
 *
 * OS Concept (Week 6): Signals and asynchronous control.
 *
 * On Linux we use signalfd(2):
 *   1. Block SIGINT and SIGTERM in the process signal mask using
 *      sigprocmask(). This prevents the default handler from firing.
 *   2. Create a signalfd that watches for those blocked signals.
 *   3. When a signal arrives, the signalfd becomes readable.
 *   4. The main loop (or epoll) reads from the signalfd and initiates
 *      a clean shutdown — no unsafe operations in a signal handler.
 *
 * On non-Linux POSIX (macOS for development):
 *   We use the classic self-pipe trick. A minimal signal handler writes
 *   one byte to a pipe. The read end of the pipe is returned as the fd.
 *   write(2) is guaranteed async-signal-safe by POSIX.
 */


#define _POSIX_C_SOURCE 200809L

#include "signal_handler.h"
#include "logger.h"

#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>

/* ─── Global shutdown flag ────────────────────────────────────────────────── */
volatile atomic_int g_shutdown_requested = ATOMIC_VAR_INIT(0);

/* ─── Platform selection ──────────────────────────────────────────────────── */
#ifdef __linux__
#  include <sys/signalfd.h>
#  define USE_SIGNALFD 1
#else
#  define USE_SIGNALFD 0
   /* Self-pipe write end — only used on non-Linux */
   static int selfpipe_write_fd = -1;

   static void selfpipe_handler(int signo)
   {
       (void)signo;
       atomic_store(&g_shutdown_requested, 1);
       /* Write one byte — async-signal-safe */
       char b = 1;
       ssize_t r;
       do { r = write(selfpipe_write_fd, &b, 1); } while (r < 0 && errno == EINTR);
   }
#endif

/* ─── signalfd implementation (Linux) ────────────────────────────────────── */
#if USE_SIGNALFD

int signal_handler_init(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    /*
     * Block SIGINT/SIGTERM in the process signal mask so they are not
     * delivered to a signal handler. Instead, they will be queued and
     * readable via the signalfd.
     */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_SYSERR("sigprocmask");
        return -1;
    }

    /*
     * Create a signalfd. SFD_NONBLOCK | SFD_CLOEXEC:
     *   - NONBLOCK: read() returns immediately if no signal is pending.
     *   - CLOEXEC: automatically closed on exec() (good hygiene).
     */
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        LOG_SYSERR("signalfd");
        return -1;
    }

    LOG_DEBUG("signalfd created: fd=%d", sfd);
    return sfd;
}

void signal_handler_cleanup(int signal_fd)
{
    if (signal_fd >= 0) close(signal_fd);

    /* Restore default signal dispositions */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

int signal_handler_check(int signal_fd)
{
    if (signal_fd < 0) return atomic_load(&g_shutdown_requested);

    struct signalfd_siginfo si;
    ssize_t r = read(signal_fd, &si, sizeof(si));
    if (r == sizeof(si)) {
        LOG_INFO("Received signal %u — initiating graceful shutdown",
                 si.ssi_signo);
        atomic_store(&g_shutdown_requested, 1);
        return 1;
    }
    return atomic_load(&g_shutdown_requested);
}

/* ─── Self-pipe implementation (non-Linux) ────────────────────────────────── */
#else /* !USE_SIGNALFD */

int signal_handler_init(void)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        LOG_SYSERR("pipe");
        return -1;
    }

    /* Make write end non-blocking (so the handler never blocks) */
    int flags = fcntl(pipefd[1], F_GETFL, 0);
    fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK);

    /* Make read end non-blocking and close-on-exec */
    flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    selfpipe_write_fd = pipefd[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = selfpipe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT,  &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_SYSERR("sigaction");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    LOG_DEBUG("self-pipe signal handler installed: read_fd=%d", pipefd[0]);
    return pipefd[0];  /* return read end */
}

void signal_handler_cleanup(int signal_fd)
{
    if (signal_fd >= 0) close(signal_fd);
    if (selfpipe_write_fd >= 0) {
        close(selfpipe_write_fd);
        selfpipe_write_fd = -1;
    }
}

int signal_handler_check(int signal_fd)
{
    if (signal_fd < 0) return atomic_load(&g_shutdown_requested);

    char buf[16];
    ssize_t r = read(signal_fd, buf, sizeof(buf));
    if (r > 0) {
        LOG_INFO("Shutdown signal received via self-pipe");
        atomic_store(&g_shutdown_requested, 1);
        return 1;
    }
    return atomic_load(&g_shutdown_requested);
}

#endif /* USE_SIGNALFD */
