/* Optional signed-linear socket RX conditioning; no private DSP offsets.
 * TX bytes are observed through const pointers, never transformed or inserted.
 * DMODEM_RX_ECHO_MODE=processed (default) reproduces the earlier processed-sample
 * pilot. DMODEM_RX_ECHO_MODE=socket counts ALL socket RX/TX, including dropped RX,
 * startup TX and delay-padding TX. Its delay is a socket INDEX separation,
 * not physical/RTP delay: the downstream bridge has a separate playout queue.
 * Noise is RMS dBFS relative to32768; echo gain is amplitude dB.
 * DMODEM_RX_SOCKET_CAUSAL=1 applies the echo scheduling constraint even to a
 * zero-echo control. Set it on both arms for a fair socket-echo comparison.
 * Noise-only socket runs need no TX lookahead and can leave it unset.
 * An unavailable/overwritten echo source is a MODEL/INSTRUMENTATION FAILURE,
 * never filled with imaginary silence and never evidence against a modem.
 * Everything defaults off. Keep a separate binary for each recorded pilot.
 */
#ifndef DMODEM_RX_CONDITION_H
#define DMODEM_RX_CONDITION_H
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef RXCOND_SYS_WRITE
#define RXCOND_SYS_WRITE write
#endif

#define RXCOND_HISTORY 4096
#define RXCOND_PROCESSED 0
#define RXCOND_SOCKET 1
typedef struct {
    int active, block, delay, mode, socket_causal;
    double noise_peak, echo_gain, rx_gain;
    uint32_t rng;
    uint64_t rx_samples, tx_samples, clipped;
    uint64_t delay_adjustments, processed_samples, skipped_samples, model_failures;
    int have_byte;
    unsigned char pending_byte;
    int16_t history[RXCOND_HISTORY];
} rxcond_state;

static int rxcond_rx(rxcond_state *, int16_t *, int);
static int rxcond_tx(rxcond_state *, const int16_t *, int);

static int rxcond_model_failure(rxcond_state *s, const char *why)
{
    ++s->model_failures;
    fprintf(stderr, "rxcond: MODEL FAILURE %s raw_rx=%llu raw_tx=%llu delay=%d\n",
            why, (unsigned long long)s->rx_samples,
            (unsigned long long)s->tx_samples, s->delay);
    errno = EPROTO;
    return -1;
}

/* Do not read a block whose delayed reference requires future TX. Smaller
 * reads allow a partial host skip to reduce lookahead without inventing data.
 * If RX skipping consumes ALL lookahead, no causal progress is possible under
 * this model; fail explicitly rather than deadlock or modify the TX policy.
 */
static int rxcond_read_budget(rxcond_state *s, int requested)
{
    uint64_t available;
    if (requested > s->block) requested = s->block;
    if (requested < 1) { errno = EINVAL; return -1; }
    if (s->mode != RXCOND_SOCKET || !s->socket_causal) return requested;
    if (s->rx_samples >= s->tx_samples + (uint64_t)s->delay)
        return rxcond_model_failure(s, "causal TX lookahead exhausted");
    available = s->tx_samples + (uint64_t)s->delay - s->rx_samples;
    if ((uint64_t)requested > available) requested = (int)available;
    return requested;
}

/* SOCK_STREAM has no sample boundaries. Keep an odd trailing byte until its
 * partner arrives; EOF halfway through a sample is an error, never silence.
 * This helper is used only by the explicitly selected experimental socket path.
 */
static int rxcond_read(rxcond_state *s, int fd, char *buf, int samples)
{
    int total = 0;
    ssize_t n;
    samples = rxcond_read_budget(s, samples);
    if (samples < 0) return -1;
    if (s->have_byte) {
        buf[total++] = (char)s->pending_byte;
        s->have_byte = 0;
    }
    do {
        n = read(fd, buf + total, (size_t)samples * 2 - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (total) { errno = EPROTO; return -1; }
            return (int)n;
        }
        total += (int)n;
    } while (total < 2);
    if (total & 1) {
        s->pending_byte = (unsigned char)buf[--total];
        s->have_byte = 1;
    }
    total /= 2;
    if (s->mode == RXCOND_SOCKET && rxcond_rx(s, (int16_t *)buf, total) < 0)
        return -1;
    return total;
}

static int rxcond_number(const char *key, double fallback, double lo, double hi,
                         double *out)
{
    const char *s = getenv(key);
    char *end;
    *out = fallback;
    if (!s || !*s) return 0;
    errno = 0;
    *out = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(*out) || *out < lo || *out > hi) {
        fprintf(stderr, "rxcond: invalid %s\n", key);
        return -1;
    }
    return 1;
}

static int rxcond_init(rxcond_state *s, int sample_rate)
{
    double enabled, noise, echo, delay, gain, causal;
    const char *mode;
    int hn, he;
    memset(s, 0, sizeof(*s));
    if (rxcond_number("DMODEM_RXCOND", 0, 0, 1, &enabled) < 0) return -1;
    if (enabled != 0 && enabled != 1) return -1;
    if (!enabled) return 0;
    mode = getenv("DMODEM_RX_ECHO_MODE");
    if (!mode || !*mode || !strcmp(mode, "processed")) s->mode = RXCOND_PROCESSED;
    else if (!strcmp(mode, "socket")) s->mode = RXCOND_SOCKET;
    else { fprintf(stderr, "rxcond: invalid DMODEM_RX_ECHO_MODE\n"); return -1; }
    hn = rxcond_number("DMODEM_RX_NOISE_DBFS", -120, -120, -20, &noise);
    he = rxcond_number("DMODEM_RX_ECHO_DB", -120, -120, -10, &echo);
    if (hn < 0 || he < 0 ||
        rxcond_number("DMODEM_RX_ECHO_SAMPLES", sample_rate / 50,
                      1, RXCOND_HISTORY, &delay) < 0 ||
        rxcond_number("DMODEM_RX_GAIN_DB", 0, -24, 0, &gain) < 0)
        return -1;
    if (delay != floor(delay) || sample_rate < 50) return -1;
    s->active = 1;
    s->delay = (int)delay;
    s->block = sample_rate / 50;
    if (s->block > s->delay) s->block = s->delay;
    s->noise_peak = hn ? 32768.0 * pow(10.0, noise / 20.0) * sqrt(3.0) : 0;
    s->echo_gain = he ? pow(10.0, echo / 20.0) : 0;
    if (rxcond_number("DMODEM_RX_SOCKET_CAUSAL", s->echo_gain != 0,
                      0, 1, &causal) < 0 || causal != floor(causal)) return -1;
    s->socket_causal = (int)causal;
    if (s->mode == RXCOND_SOCKET && s->echo_gain && !s->socket_causal) {
        fprintf(stderr, "rxcond: socket echo requires causal scheduling\n");
        return -1;
    }
    s->rx_gain = pow(10.0, gain / 20.0);
    s->rng = 0x6d2b79f5U;
    fprintf(stderr, "rxcond: mode=%s causal=%d rate=%d block=%d delay=%d noise_peak=%.9g "
            "echo_gain=%.9g rx_gain=%.9g seed=%u\n",
            s->mode == RXCOND_SOCKET ? "socket-index" : "processed", s->socket_causal,
            sample_rate, s->block,
            s->delay, s->noise_peak, s->echo_gain, s->rx_gain, s->rng);
    return 0;
}

static double rxcond_noise(rxcond_state *s)
{
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return ((double)x / 4294967296.0 * 2.0 - 1.0) * s->noise_peak;
}

static int rxcond_rx(rxcond_state *s, int16_t *samples, int count)
{
    int i;
    if (!s->active) return 0;
    if (count < 0 || count > s->block ||
        (s->mode == RXCOND_PROCESSED && s->rx_samples != s->tx_samples))
        return -1;
    /* Validate the whole block BEFORE changing one byte or advancing RNG. */
    if ((s->echo_gain || (s->mode == RXCOND_SOCKET && s->socket_causal)) && count) {
        uint64_t last = s->rx_samples + (uint64_t)count - 1;
        if (last >= (uint64_t)s->delay) {
            uint64_t first = s->rx_samples >= (uint64_t)s->delay ?
                             s->rx_samples - (uint64_t)s->delay : 0;
            if (last - (uint64_t)s->delay >= s->tx_samples)
                return rxcond_model_failure(s, "requested echo TX has not been written");
            if (s->tx_samples - first > RXCOND_HISTORY)
                return rxcond_model_failure(s, "requested echo TX history was overwritten");
        }
    }
    for (i = 0; i < count; ++i) {
        uint64_t at = s->rx_samples + (uint64_t)i;
        double value = samples[i] * s->rx_gain;
        if (s->echo_gain && at >= (uint64_t)s->delay) {
            uint64_t source = at - (uint64_t)s->delay;
            value += s->history[source % RXCOND_HISTORY] * s->echo_gain;
        }
        if (s->noise_peak) value += rxcond_noise(s);
        if (value > 32767) { value = 32767; ++s->clipped; }
        if (value < -32768) { value = -32768; ++s->clipped; }
        samples[i] = (int16_t)lrint(value);
    }
    s->rx_samples += (uint64_t)count;
    return 0;
}

static int rxcond_tx(rxcond_state *s, const int16_t *samples, int count)
{
    int i;
    if (!s->active) return 0;
    if (count < 0 || (s->mode == RXCOND_PROCESSED &&
                     s->tx_samples + (uint64_t)count != s->rx_samples)) return -1;
    for (i = 0; i < count; ++i)
        s->history[(s->tx_samples + (uint64_t)i) % RXCOND_HISTORY] = samples[i];
    s->tx_samples += (uint64_t)count;
    return 0;
}

/* Socket mode records only fully successful writes, including startup and
 * padding through the same function. Partial/EINTR writes keep byte order and
 * retry the unsent suffix; they never duplicate a complete sample or add TX.
 * The earlier processed mode retains its one-write/short-write-fails policy.
 */
static int rxcond_write(rxcond_state *s, int fd, const char *buf, int samples)
{
    size_t done = 0, bytes;
    ssize_t n;
    if (samples < 0) { errno = EINVAL; return -1; }
    bytes = (size_t)samples * 2;
    if (s->mode == RXCOND_PROCESSED) {
        n = RXCOND_SYS_WRITE(fd, buf, bytes);
        if (n >= 0 && (size_t)n != bytes) { errno = EIO; return -1; }
        return n < 0 ? (int)n : (int)n / 2;
    }
    while (done < bytes) {
        n = RXCOND_SYS_WRITE(fd, buf + done, bytes - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    if (rxcond_tx(s, (const int16_t *)buf, samples) < 0) return -1;
    return samples;
}

/* The host's old `void *in -= update_delay` both used byte arithmetic and was
 * ignored by modem_process(). Correct only the selected socket experiment:
 * raw RX is already counted/conditioned, so a dropped block still advances the
 * reference clock, while the retained suffix begins at a true int16_t offset.
 */
static int16_t *rxcond_skip_input(rxcond_state *s, int16_t *buf, int *count,
                                  int *update_delay, int *device_delay)
{
    int skipped;
    if (*count < 0 || *update_delay >= 0) return buf;
    skipped = *update_delay <= -*count ? *count : -*update_delay;
    *count -= skipped;
    *update_delay += skipped;
    *device_delay -= skipped;
    s->skipped_samples += (uint64_t)skipped;
    return buf + skipped;
}
#endif
