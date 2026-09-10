/* Optional socket-DSP receive conditioning. No vendor object offsets.
 * Samples are signed linear PCM at the DSP rate, before modem_process().
 * TX is observed through a const pointer and never changed. Noise level is
 * RMS dBFS relative to 32768; echo gain is amplitude dB, not dBFS. The echo
 * delay is in processed DSP samples, not a claim about physical line delay.
 * Experimental controls default off; DMODEM_RXCOND=1 selects an unconditioned
 * control with the same bounded processing blocks as the conditioned arms.
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

#define RXCOND_HISTORY 4096
typedef struct {
    int active, block, delay;
    double noise_peak, echo_gain, rx_gain;
    uint32_t rng;
    uint64_t rx_samples, tx_samples, clipped;
    uint64_t delay_adjustments;
    int have_byte;
    unsigned char pending_byte;
    int16_t history[RXCOND_HISTORY];
} rxcond_state;

/* SOCK_STREAM has no sample boundaries. Keep an odd trailing byte until its
 * partner arrives; EOF halfway through a sample is an error, never silence.
 * This helper is used only by the explicitly selected experimental socket path.
 */
static int rxcond_read(rxcond_state *s, int fd, char *buf, int samples)
{
    int total = 0;
    ssize_t n;
    if (samples > s->block) samples = s->block;
    if (samples < 1) { errno = EINVAL; return -1; }
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
    return total / 2;
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
    double enabled, noise, echo, delay, gain;
    int hn, he;
    memset(s, 0, sizeof(*s));
    if (rxcond_number("DMODEM_RXCOND", 0, 0, 1, &enabled) < 0) return -1;
    if (enabled != 0 && enabled != 1) return -1;
    if (!enabled) return 0;
    hn = rxcond_number("DMODEM_RX_NOISE_DBFS", -120, -120, -20, &noise);
    he = rxcond_number("DMODEM_RX_ECHO_DB", -120, -120, -10, &echo);
    if (hn < 0 || he < 0 ||
        rxcond_number("DMODEM_RX_ECHO_SAMPLES", sample_rate / 50,
                      1, RXCOND_HISTORY, &delay) < 0 ||
        rxcond_number("DMODEM_RX_GAIN_DB", 0, -24, 0, &gain) < 0)
        return -1;
    if (delay != floor(delay) || sample_rate <= 0) return -1;
    s->active = 1;
    s->delay = (int)delay;
    s->block = sample_rate / 50;
    if (s->block > s->delay) s->block = s->delay;
    s->noise_peak = hn ? 32768.0 * pow(10.0, noise / 20.0) * sqrt(3.0) : 0;
    s->echo_gain = he ? pow(10.0, echo / 20.0) : 0;
    s->rx_gain = pow(10.0, gain / 20.0);
    s->rng = 0x6d2b79f5U;
    fprintf(stderr, "rxcond: rate=%d block=%d delay=%d noise_peak=%.9g "
            "echo_gain=%.9g rx_gain=%.9g seed=%u\n", sample_rate, s->block,
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
    if (count < 0 || count > s->block || s->rx_samples != s->tx_samples)
        return -1;
    for (i = 0; i < count; ++i) {
        uint64_t at = s->rx_samples + (uint64_t)i;
        double value = samples[i] * s->rx_gain;
        if (s->echo_gain && at >= (uint64_t)s->delay) {
            uint64_t source = at - (uint64_t)s->delay;
            if (source >= s->tx_samples || s->tx_samples - source > RXCOND_HISTORY)
                return -1;
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
    if (count < 0 || s->tx_samples + (uint64_t)count != s->rx_samples) return -1;
    for (i = 0; i < count; ++i)
        s->history[(s->tx_samples + (uint64_t)i) % RXCOND_HISTORY] = samples[i];
    s->tx_samples += (uint64_t)count;
    return 0;
}
#endif
