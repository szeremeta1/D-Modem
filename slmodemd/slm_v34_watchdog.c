/*
 * Opt-in observation/override of the V.34 handshake carrier-loss criterion.
 *
 * In the exact i386 dsplibs.o accepted by apply_watchdog_hook.sh, v34handshak
 * increments obj+0x234 while its signed 16-bit metric is below the signed
 * 32-bit threshold at obj+0x230. A good sample resets the counter; 9600
 * consecutive low samples set status 9 at the 9600 Hz DSP rate. These are
 * private ABI offsets, not a portable API. Never bypass the installer's hash
 * gate when using a different vendor object.
 *
 * SLM_V34_LOWSIG=-1000000 makes that comparison false for every int16 metric.
 * Unset (or the historical control value 1) preserves the vendor threshold.
 * This is a measured workaround for one path, not a correction to the
 * vendor's default threshold or proof of the underlying analog-model cause.
 * Other handshake timeouts remain; this hook does not improve negotiated
 * speed, implement analog self-echo, or establish a connection reliability SLA.
 *
 * SLM_V34_METRIC=N optionally logs every N wrapper invocations. The fragment
 * index is process-wide, not a call identifier or a wall clock. No diagnostic
 * is emitted and no private object field is accessed when both knobs are off.
 * The hook has no per-call interventions or counters that can survive allocator
 * address reuse, and does not call vendor rate/SNR getters on fatal states.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__i386__) && !defined(SLM_WATCHDOG_TEST)
#error "This wrapper supports only the installer's verified i386 DSP ABI"
#endif

typedef char slm_watchdog_abi_sizes[
    sizeof(int) == 4 && sizeof(short) == 2 && sizeof(float) == 4 ? 1 : -1];

extern int slm_v34_watchdog_orig(void *, float *, float *, int,
                                int *, int *, int *, int *)
    __asm__("VPcmV34Progress__orig");
int slm_v34_watchdog_wrap(void *, float *, float *, int,
                          int *, int *, int *, int *)
    __asm__("VPcmV34Progress");

/* Invalid settings fail closed: retain the original criterion. Configuration
 * is read once because slmodemd has one datapump per process and the host sets
 * these variables before startup. Never include the supplied value in logs. */
static int read_setting(const char *name, int *value)
{
    const char *s = getenv(name);
    char *end;
    long parsed;
    if (!s || !*s) return 0;
    errno = 0;
    parsed = strtol(s, &end, 10);
    if (errno || end == s || *end || parsed < INT_MIN || parsed > INT_MAX) {
        fprintf(stderr, "V34PROG invalid %s; disabled\n", name);
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

int slm_v34_watchdog_wrap(void *obj, float *in, float *out, int n,
                          int *rxbits, int *nrx, int *txbits, int *ntx)
{
    static int configured, override, threshold, metric_interval;
    static unsigned long fragments;
    int result;
    if (!configured) {
        override = read_setting("SLM_V34_LOWSIG", &threshold) && threshold != 1;
        if (!read_setting("SLM_V34_METRIC", &metric_interval) || metric_interval < 1)
            metric_interval = 0;
        configured = 1;
    }
    if (!override && !metric_interval)
        return slm_v34_watchdog_orig(obj, in, out, n, rxbits, nrx, txbits, ntx);

    /* Reapply before each fragment because the vendor initializes the
     * threshold again when it re-enters the handshake. memcpy avoids imposing
     * an effective C struct type on storage owned by the vendor. */
    if (override)
        memcpy((char *)obj + 0x230, &threshold, sizeof threshold);
    result = slm_v34_watchdog_orig(obj, in, out, n, rxbits, nrx, txbits, ntx);
    if (metric_interval && ++fragments % (unsigned)metric_interval == 0) {
        short metric;
        int current_threshold, low_count;
        memcpy(&metric, (char *)obj + 0x398, sizeof metric);
        memcpy(&current_threshold, (char *)obj + 0x230, sizeof current_threshold);
        memcpy(&low_count, (char *)obj + 0x234, sizeof low_count);
        fprintf(stderr, "V34METRIC fragment %lu metric %d threshold %d "
                "lowcnt %d status %d\n", fragments, (int)metric,
                current_threshold, low_count, result);
    }
    return result;
}
