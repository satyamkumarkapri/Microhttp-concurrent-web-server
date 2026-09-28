/*
 * timer.h — Idle Connection Timeout Interface
 *
 * OS Concept (Week 6, 12):
 *   timerfd_create(2) creates a file descriptor that fires when a timer
 *   expires. This allows timer events to be integrated into an epoll event
 *   loop without separate timer threads. Each fired event is a read() on
 *   the timerfd.
 *
 *   In thread-pool mode we use a simpler approach: a dedicated timeout
 *   thread that periodically scans connection timestamps.
 *
 * These functions are Linux-specific. They are guarded by #ifdef __linux__.
 * On macOS the timeout thread approach is used.
 */

#ifndef MICROHTTP_TIMER_H
#define MICROHTTP_TIMER_H

#include <time.h>
#include <sys/types.h>
#include <stdint.h>

/*
 * timer_create_idle — create a timerfd set to fire every 'interval_sec' seconds.
 *
 * Returns the timerfd file descriptor, or -1 on error.
 * Only available on Linux.
 */
int timer_create_idle(int interval_sec);

/*
 * timer_reset — arm/rearm a timerfd to fire after 'seconds' seconds.
 * Returns 0 on success, -1 on error.
 */
int timer_reset(int timerfd, int seconds);

/*
 * timer_read — consume a timerfd expiry event.
 * Call after epoll reports the timerfd is readable.
 * Returns the expiry count (usually 1), or 0 on EAGAIN.
 */
uint64_t timer_read(int timerfd);

#endif /* MICROHTTP_TIMER_H */
