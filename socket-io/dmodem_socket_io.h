/* Opt-in signed-16-bit socket transport repair. No DSP/sample transforms,
 * resampling, read cap, periodic clock or packet-size requirement.
 * Linux: compile with -D_GNU_SOURCE for ppoll. */
#ifndef DMODEM_SOCKET_IO_H
#define DMODEM_SOCKET_IO_H
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SIO_RECV
#define SIO_RECV recv
#endif
#ifndef SIO_SEND
#define SIO_SEND send
#endif
typedef int (*sio_cancel_fn)(void);
typedef struct {
    int enabled, pending;
    unsigned char byte;
    sio_cancel_fn cancelled;
    uint64_t rx_bytes, tx_bytes, odd_reads, short_writes;
    uint64_t read_eintr, write_eintr, read_waits, write_waits;
    uint64_t io_errors, cancelled_io, instrument_errors, truncated_samples;
    uint64_t skipped_samples, skip_requests;
} sio_state;

static int sio_cancel(sio_state *s)
{
    if (s->cancelled && s->cancelled()) {
        ++s->cancelled_io;
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

#ifndef SIO_WAIT
/* Block termination signals only around the cancellation check. ppoll atomically
 * restores the caller's mask while sleeping, closing the check/wait race. No
 * timeout or busy polling. The descriptor itself stays in its original mode. */
static int sio_wait(sio_state *s, int fd, short events)
{
    sigset_t term, old;
    struct pollfd p = { .fd = fd, .events = events };
    int rc, saved;
    sigemptyset(&term);
    sigaddset(&term, SIGINT);
    sigaddset(&term, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &term, &old) < 0) return -1;
    rc = sio_cancel(s);
    if (rc == 0) rc = ppoll(&p, 1, NULL, &old);
    saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    errno = saved;
    if (rc > 0 && (p.revents & POLLNVAL)) { errno = EBADF; return -1; }
    return rc;
}
#define SIO_WAIT sio_wait
#endif

static int sio_init(sio_state *s, const char *value, sio_cancel_fn cancelled)
{
    memset(s, 0, sizeof(*s));
    s->cancelled = cancelled;
    if (!value || !strcmp(value, "0")) return 0;
    if (strcmp(value, "1")) {
        ++s->instrument_errors;
        errno = EINVAL;
        return -1;
    }
    s->enabled = 1;
    return 0;
}

static int sio_invalid(sio_state *s)
{
    ++s->instrument_errors;
    errno = EINVAL;
    return -1;
}

static int sio_read(sio_state *s, int fd, char *buf, int samples)
{
    size_t total = 0, capacity;
    if (samples <= 0 || samples > INT_MAX/2) return sio_invalid(s);
    capacity = (size_t)samples * 2;
    if (s->pending) { buf[total++] = (char)s->byte; s->pending = 0; }
    do {
        ssize_t n;
        if (sio_cancel(s) < 0) return -1;
        n = SIO_RECV(fd, buf + total, capacity - total, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) { ++s->read_eintr; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc;
            ++s->read_waits;
            rc = SIO_WAIT(s, fd, POLLIN);
            if (rc < 0 && errno == EINTR) { ++s->read_eintr; continue; }
            if (rc > 0) continue;
            if (errno != ECANCELED) ++s->io_errors;
            return -1;
        }
        if (n < 0) { ++s->io_errors; return -1; }
        if (n == 0) {
            if (total) {
                ++s->truncated_samples;
                ++s->io_errors;
                errno = EPROTO;
                return -1;
            }
            return 0;
        }
        s->rx_bytes += (uint64_t)n;
        if (n & 1) ++s->odd_reads;
        total += (size_t)n;
    } while (total < 2);
    if (total & 1) { s->byte = (unsigned char)buf[--total]; s->pending = 1; }
    return (int)(total / 2);
}

static int sio_write(sio_state *s, int fd, const char *buf, int samples)
{
    size_t done = 0, bytes;
    if (samples < 0 || samples > INT_MAX/2) return sio_invalid(s);
    bytes = (size_t)samples * 2;
    while (done < bytes) {
        ssize_t n;
        if (sio_cancel(s) < 0) return -1;
        n = SIO_SEND(fd, buf + done, bytes - done, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) { ++s->write_eintr; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc;
            ++s->write_waits;
            rc = SIO_WAIT(s, fd, POLLOUT);
            if (rc < 0 && errno == EINTR) { ++s->write_eintr; continue; }
            if (rc > 0) continue;
            if (errno != ECANCELED) ++s->io_errors;
            return -1;
        }
        if (n <= 0) {
            ++s->io_errors;
            if (!n) errno = EIO;
            return -1;
        }
        if ((size_t)n < bytes - done) ++s->short_writes;
        done += (size_t)n;
        s->tx_bytes += (uint64_t)n;
    }
    return samples;
}

/* On error, all caller-owned pointers/counters remain unchanged. */
static int sio_skip(sio_state *s, void **input, int *count,
                    int *adjustment, int *delay)
{
    int64_t skip, next_delay;
    if (*adjustment >= 0) return 0;
    if (*count <= 0) return sio_invalid(s);
    skip = -(int64_t)*adjustment;
    if (skip > *count) skip = *count;
    next_delay = (int64_t)*delay - skip;
    if (next_delay < INT_MIN || next_delay > INT_MAX) return sio_invalid(s);
    *input = (char *)*input + (size_t)skip * 2;
    *count -= (int)skip;
    *adjustment += (int)skip;
    *delay = (int)next_delay;
    ++s->skip_requests;
    s->skipped_samples += (uint64_t)skip;
    return 0;
}

static int sio_padding(sio_state *s, int samples, size_t capacity, int delay)
{
    if (samples < 0 || (size_t)samples > capacity/2 ||
        (int64_t)delay + samples > INT_MAX) return sio_invalid(s);
    return 0;
}

static void sio_report(const sio_state *s)
{
    fprintf(stderr, "socket_io: enabled=%d rx_bytes=%llu tx_bytes=%llu "
        "odd_reads=%llu short_writes=%llu read_eintr=%llu write_eintr=%llu "
        "read_waits=%llu write_waits=%llu io_errors=%llu cancelled=%llu "
        "instrument_errors=%llu truncated_samples=%llu skipped=%llu skip_requests=%llu\n",
        s->enabled, (unsigned long long)s->rx_bytes, (unsigned long long)s->tx_bytes,
        (unsigned long long)s->odd_reads, (unsigned long long)s->short_writes,
        (unsigned long long)s->read_eintr, (unsigned long long)s->write_eintr,
        (unsigned long long)s->read_waits, (unsigned long long)s->write_waits,
        (unsigned long long)s->io_errors, (unsigned long long)s->cancelled_io,
        (unsigned long long)s->instrument_errors, (unsigned long long)s->truncated_samples,
        (unsigned long long)s->skipped_samples, (unsigned long long)s->skip_requests);
}
#endif
