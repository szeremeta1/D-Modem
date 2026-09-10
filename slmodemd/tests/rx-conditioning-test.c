/* cc -O2 -Wall -Wextra slmodemd/tests/rx-conditioning-test.c -lm -o /tmp/rxcond-test */
#include "../dmodem_rx_condition.h"
#include <assert.h>

static void clear_config(void)
{
    unsetenv("DMODEM_RXCOND"); unsetenv("DMODEM_RX_NOISE_DBFS");
    unsetenv("DMODEM_RX_ECHO_DB"); unsetenv("DMODEM_RX_ECHO_SAMPLES");
    unsetenv("DMODEM_RX_GAIN_DB");
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
    puts("PASS: bypass, control, noise RMS, delayed echo, TX preservation, clipping, reset, invalid settings");
    return 0;
}
