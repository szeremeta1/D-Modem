/* cc -O2 -Wall -Wextra slmodemd/tests/rx-conditioning-test.c -lm -o /tmp/rxcond-test */
#include <unistd.h>
static ssize_t fragmented_write(int fd, const void *buf, size_t count);
#define RXCOND_SYS_WRITE fragmented_write
#include "../dmodem_rx_condition.h"
#include <assert.h>

static int fragment_writes, interrupt_write, write_calls;
static ssize_t fragmented_write(int fd, const void *buf, size_t count)
{
    ++write_calls;
    if (interrupt_write) { interrupt_write = 0; errno = EINTR; return -1; }
    if (fragment_writes && count > 3) count = 3;
    return write(fd, buf, count);
}

static void clear_config(void)
{
    unsetenv("DMODEM_RXCOND"); unsetenv("DMODEM_RX_NOISE_DBFS");
    unsetenv("DMODEM_RX_ECHO_DB"); unsetenv("DMODEM_RX_ECHO_SAMPLES");
    unsetenv("DMODEM_RX_GAIN_DB");
    unsetenv("DMODEM_RX_ECHO_MODE"); unsetenv("DMODEM_RX_SOCKET_CAUSAL");
}

static void socket_config(int echo)
{
    clear_config();
    setenv("DMODEM_RXCOND", "1", 1);
    setenv("DMODEM_RX_ECHO_MODE", "socket", 1);
    if (echo) setenv("DMODEM_RX_ECHO_DB", "-40", 1);
}

static void socket_tests(void)
{
    rxcond_state s;
    int16_t rx[192], tx[384], copied[384], sink[384];
    int fds[2], i, count, update, delay;
    int16_t *kept;
    socket_config(1);
    assert(rxcond_init(&s, 9600) == 0 && s.mode == RXCOND_SOCKET && s.socket_causal);
    assert(pipe(fds) == 0);
    memset(tx, 0, sizeof tx);
    /* Startup's real 192 zero samples must occupy TX indices 0..191. */
    assert(rxcond_write(&s, fds[1], (const char *)tx, 192) == 192);
    assert(read(fds[0], sink, 384) == 384 && s.tx_samples == 192);
    memset(rx, 0, sizeof rx);
    assert(rxcond_rx(&s, rx, 192) == 0);
    tx[0] = 10000;
    memcpy(copied, tx, sizeof tx);
    assert(rxcond_write(&s, fds[1], (const char *)tx, 192) == 192);
    assert(read(fds[0], sink, 384) == 384 && !memcmp(tx, copied, sizeof tx));
    memset(rx, 0, sizeof rx);
    assert(rxcond_rx(&s, rx, 192) == 0);
    for (i = 0; i < 192; ++i) assert(rx[i] == 0);
    /* A host +384 padding write occupies actual TX indices 384..767. */
    memset(tx, 0, sizeof tx);
    assert(rxcond_write(&s, fds[1], (const char *)tx, 384) == 384);
    assert(read(fds[0], sink, 768) == 768 && s.tx_samples == 768);
    memset(rx, 0, sizeof rx);
    assert(rxcond_rx(&s, rx, 192) == 0 && rx[0] == 100);
    for (i = 1; i < 192; ++i) assert(rx[i] == 0);
    for (i = 0; i < 2; ++i) {
        memset(rx, 0, sizeof rx);
        assert(rxcond_rx(&s, rx, 192) == 0);
        assert(!memcmp(rx, tx, sizeof rx));
    }
    assert(s.rx_samples == 960);
    assert(rxcond_read_budget(&s, 192) == -1 && s.model_failures == 1);
    close(fds[0]); close(fds[1]);

    /* Noise-only socket mode advances on dropped input independently of TX. */
    socket_config(0);
    assert(rxcond_init(&s, 9600) == 0 && !s.socket_causal);
    for (i = 0; i < 192; ++i) rx[i] = (int16_t)i;
    assert(rxcond_rx(&s, rx, 192) == 0);
    count = 192; update = -272; delay = 512;
    kept = rxcond_skip_input(&s, rx, &count, &update, &delay);
    assert(kept == rx + 192 && count == 0 && update == -80 && delay == 320);
    for (i = 0; i < 192; ++i) rx[i] = (int16_t)(192 + i);
    assert(rxcond_rx(&s, rx, 192) == 0);
    count = 192;
    kept = rxcond_skip_input(&s, rx, &count, &update, &delay);
    assert(kept == rx + 80 && kept[0] == 272 && count == 112 && update == 0 && delay == 240);
    s.processed_samples += (uint64_t)count;
    assert(s.rx_samples == 384 && s.skipped_samples == 272 && s.processed_samples == 112);
    assert(rxcond_read_budget(&s, 192) == 192); /* no echo-imposed clock */

    /* Causal echo keeps progressing with a 112-sample read after the same
     * skip, but explicitly refuses a later complete exhaustion of lookahead. */
    socket_config(1);
    assert(rxcond_init(&s, 9600) == 0);
    memset(tx, 0, sizeof tx);
    assert(rxcond_tx(&s, tx, 384) == 0);
    memset(rx, 0, sizeof rx);
    assert(rxcond_rx(&s, rx, 192) == 0);
    assert(rxcond_rx(&s, rx, 192) == 0); /* full dropped block */
    assert(rxcond_rx(&s, rx, 192) == 0); /* partial dropped block */
    assert(rxcond_tx(&s, tx, 112) == 0);
    assert(rxcond_read_budget(&s, 192) == 112);
    assert(pipe(fds) == 0);
    assert(write(fds[1], tx, 384) == 384);
    assert(rxcond_read(&s, fds[0], (char *)rx, 192) == 112);
    assert(s.rx_samples == 688);
    assert(rxcond_read_budget(&s, 192) == -1 && s.model_failures == 1);
    close(fds[0]); close(fds[1]);

    /* Stream splitting changes no samples and advances raw RX on every read. */
    socket_config(0);
    assert(rxcond_init(&s, 9600) == 0);
    assert(pipe(fds) == 0);
    assert(write(fds[1], "abc", 3) == 3);
    assert(rxcond_read(&s, fds[0], (char *)rx, 192) == 1 && s.rx_samples == 1);
    assert(write(fds[1], "def", 3) == 3);
    assert(rxcond_read(&s, fds[0], (char *)rx, 192) == 2 && s.rx_samples == 3);
    assert(!memcmp(rx, "cdef", 4));
    close(fds[0]); close(fds[1]);

    /* Odd partial writes + EINTR must preserve every TX byte and append the
     * history once; no zero insertion, repeated prefix or modified sample. */
    assert(pipe(fds) == 0);
    tx[0] = 0x1234; tx[1] = -1234; tx[2] = 32767; tx[3] = -32768; tx[4] = 7;
    memcpy(copied, tx, sizeof tx);
    fragment_writes = interrupt_write = 1; write_calls = 0;
    assert(rxcond_write(&s, fds[1], (const char *)tx, 5) == 5);
    assert(write_calls == 5 && s.tx_samples == 5);
    assert(read(fds[0], sink, 10) == 10 && !memcmp(sink, copied, 10));
    assert(!memcmp(tx, copied, sizeof tx));
    fragment_writes = 0;
    close(fds[0]); close(fds[1]);

    socket_config(1);
    assert(rxcond_init(&s, 9600) == 0);
    s.rx_samples = 192; s.tx_samples = RXCOND_HISTORY + 1;
    memset(rx, 0x11, sizeof rx); memcpy(copied, rx, sizeof rx);
    assert(rxcond_rx(&s, rx, 1) == -1 && s.model_failures == 1);
    assert(!memcmp(rx, copied, sizeof rx));

    socket_config(0);
    setenv("DMODEM_RX_SOCKET_CAUSAL", "1", 1);
    assert(rxcond_init(&s, 9600) == 0 && s.socket_causal && !s.echo_gain);
    s.rx_samples = 192;
    assert(rxcond_read_budget(&s, 192) == -1); /* identical control scheduler */
    socket_config(1);
    setenv("DMODEM_RX_SOCKET_CAUSAL", "0", 1);
    assert(rxcond_init(&s, 9600) == -1);
}

int main(void)
{
    rxcond_state s;
    int16_t rx[192], tx[192], original[192];
    int i, block;
    double sum = 0, sumsq = 0, cross = 0, last = 0;
    clear_config();
    for (i = 0; i < 192; ++i) original[i] = rx[i] = tx[i] = (int16_t)(i * 311 - 30000);
    assert(rxcond_init(&s, 9600) == 0 && !s.active);
    assert(rxcond_rx(&s, rx, 192) == 0);
    assert(rxcond_tx(&s, tx, 192) == 0);
    assert(!memcmp(rx, original, sizeof rx));
    assert(!memcmp(tx, original, sizeof tx));

    setenv("DMODEM_RXCOND", "1", 1);
    assert(rxcond_init(&s, 9600) == 0 && s.active && s.block == 192);
    assert(rxcond_rx(&s, rx, 192) == 0 && rxcond_tx(&s, tx, 192) == 0);
    assert(!memcmp(rx, original, sizeof rx));
    assert(rxcond_rx(&s, rx, 193) == -1);

    setenv("DMODEM_RX_NOISE_DBFS", "-65", 1);
    assert(rxcond_init(&s, 9600) == 0);
    memset(tx, 0, sizeof tx);
    for (block = 0; block < 10000; ++block) {
        memset(rx, 0, sizeof rx);
        assert(rxcond_rx(&s, rx, 192) == 0);
        assert(rxcond_tx(&s, tx, 192) == 0);
        for (i = 0; i < 192; ++i) {
            sum += rx[i]; sumsq += (double)rx[i]*rx[i];
            cross += last * rx[i]; last = rx[i];
        }
    }
    printf("noise: mean %.6f RMS %.6f dBFS %.6f lag1 %.6f\n", sum/1920000,
           sqrt(sumsq/1920000), 20*log10(sqrt(sumsq/1920000)/32768), cross/sumsq);
    assert(fabs(sum/1920000) < .1);
    assert(fabs(20*log10(sqrt(sumsq/1920000)/32768) + 65) < .05);
    assert(fabs(cross/sumsq) < .005 && s.clipped == 0);

    {   /* Deliberately split signed samples across stream writes. */
        int fds[2];
        char bytes[8] = {0};
        assert(pipe(fds) == 0);
        assert(write(fds[1], "abc", 3) == 3);
        assert(rxcond_read(&s, fds[0], bytes, 192) == 1);
        assert(!memcmp(bytes, "ab", 2) && s.have_byte);
        assert(write(fds[1], "def", 3) == 3);
        assert(rxcond_read(&s, fds[0], bytes, 192) == 2);
        assert(!memcmp(bytes, "cdef", 4) && !s.have_byte);
        assert(write(fds[1], "ghi", 3) == 3);
        assert(rxcond_read(&s, fds[0], bytes, 192) == 1 && s.have_byte);
        close(fds[1]);
        assert(rxcond_read(&s, fds[0], bytes, 192) == -1 && errno == EPROTO);
        close(fds[0]);
    }

    unsetenv("DMODEM_RX_NOISE_DBFS");
    setenv("DMODEM_RX_ECHO_DB", "-40", 1);
    assert(rxcond_init(&s, 9600) == 0);
    /* Vary the processing chunk across a ring wrap; impulse every 257 samples
       must reappear 192 samples later at exactly 1/100 amplitude. */
    for (block = 0; block < 200; ++block) {
        int n = 1 + (block * 37) % 192;
        uint64_t start = s.rx_samples;
        memset(rx, 0, sizeof rx);
        for (i = 0; i < n; ++i) tx[i] = ((start + i) % 257 == 0) ? 10000 : 0;
        memcpy(original, tx, sizeof tx);
        assert(rxcond_rx(&s, rx, n) == 0);
        for (i = 0; i < n; ++i) {
            uint64_t at = start + i;
            assert(rx[i] == (at >= 192 && (at - 192) % 257 == 0 ? 100 : 0));
        }
        assert(rxcond_tx(&s, tx, n) == 0);
        assert(!memcmp(original, tx, sizeof tx));
    }
    assert(rxcond_init(&s, 9600) == 0 && s.rx_samples == 0 && s.tx_samples == 0);
    setenv("DMODEM_RX_ECHO_DB", "-10", 1);
    assert(rxcond_init(&s, 9600) == 0);
    for (i = 0; i < 192; ++i) rx[i] = tx[i] = 32767;
    assert(rxcond_rx(&s, rx, 192) == 0 && rxcond_tx(&s, tx, 192) == 0);
    assert(rxcond_rx(&s, rx, 192) == 0 && s.clipped == 192);
    for (i = 0; i < 192; ++i) assert(rx[i] == 32767);
    setenv("DMODEM_RX_ECHO_SAMPLES", "1.5", 1);
    assert(rxcond_init(&s, 9600) == -1);
    setenv("DMODEM_RX_ECHO_SAMPLES", "nan", 1);
    assert(rxcond_init(&s, 9600) == -1);
    clear_config();
    socket_tests();
    clear_config();
    puts("PASS: bypass, control, noise RMS, delayed echo, TX preservation, clipping, reset, invalid settings");
    puts("PASS: raw socket startup/padding, complete/partial skips, causal small reads, explicit model failure, fragmented writes");
    return 0;
}
