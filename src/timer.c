/*
 * timer.c — timerfd Implementation (Linux only)
 *
 * OS Concept (Week 6):
 *   timerfd_create(2): Creates a timer that fires events via a file descriptor.
 *   timerfd_settime(2): Arms the timer with an initial expiry and interval.
 *   read(2) on the timerfd: Returns a uint64_t indicating how many times
 *     the timer has fired since the last read. This is how we "consume" events.
 *
 *   This design allows the epoll event loop to handle timer events with the
 *   same epoll_wait() call used for socket I/O — no threads needed for timers.
 */


#define _POSIX_C_SOURCE 200809L

#include "timer.h"
#include "logger.h"

#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef __linux__
#  include <sys/timerfd.h>

int timer_create_idle(int interval_sec)
{
    /*
     * CLOCK_MONOTONIC: Uses a monotonic clock that is not affected by
     * system time changes (NTP adjustments, etc.). Correct for measuring
     * elapsed time.
     *
     * TFD_NONBLOCK: read() returns EAGAIN if no expiry has occurred yet.
     * TFD_CLOEXEC:  Closed automatically on exec().
     */
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        LOG_SYSERR("timerfd_create");
        return -1;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec    = interval_sec;  /* initial expiry */
    its.it_interval.tv_sec = interval_sec;  /* repeating interval */

    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        LOG_SYSERR("timerfd_settime");
        close(fd);
        return -1;
    }

    LOG_DEBUG("timerfd created: fd=%d interval=%ds", fd, interval_sec);
    return fd;
}

int timer_reset(int timerfd, int seconds)
{
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec    = seconds;
    its.it_interval.tv_sec = seconds;

    if (timerfd_settime(timerfd, 0, &its, NULL) < 0) {
        LOG_SYSERR("timerfd_settime (reset)");
        return -1;
    }
    return 0;
}

uint64_t timer_read(int timerfd)
{
    uint64_t expirations = 0;
    ssize_t r = read(timerfd, &expirations, sizeof(expirations));
    if (r < 0) {
        if (errno == EAGAIN) return 0;  /* no expiry yet */
        LOG_SYSERR("timerfd read");
    }
    return expirations;
}

#else /* !__linux__ — stub implementations for macOS compilation */

int timer_create_idle(int interval_sec)
{
    (void)interval_sec;
    LOG_WARN("timerfd not available on this platform (Linux only)");
    return -1;
}

int timer_reset(int timerfd, int seconds)
{
    (void)timerfd; (void)seconds;
    return -1;
}

uint64_t timer_read(int timerfd)
{
    (void)timerfd;
    return 0;
}

#endif /* __linux__ */
