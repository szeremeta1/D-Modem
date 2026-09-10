/* Experimental near-end V.34 echo OUTPUT bypass, exact checked i386 DSP ABI only.
 * The caller passes a signed TX-history offset, not a received audio sample.
 * Always run the vendor filter to advance its working history. Only the two
 * verified near-context callers receive zero; far and unknown callers retain
 * the vendor result. Adaptation is left enabled and may follow a different
 * trajectory when the receiver error changes. This is not a CPU-cost bypass.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

_Static_assert(sizeof(void *) == 4 && sizeof(short) == 2 && sizeof(int) == 4,
               "exact 32-bit vendor ABI required");
extern int V34EchoFilter__near_original(void *, short);
extern const char dmodem_near_data_return[], dmodem_near_training_return[];
static int bypass;

/* Read once before DSP processing. Strict opt-in; no environment work or
 * logging in the per-sample path, and no mutable process-global object pointer.
 */
__attribute__((constructor)) static void configure_near_ec(void)
{
    const char *v = getenv("DMODEM_V34_NEAR_EC_BYPASS");
    bypass = v && strcmp(v, "1") == 0;
    if (bypass)
        fputs("DMODEM: experimental near V.34 EC output bypass enabled; far unchanged\n", stderr);
}

__attribute__((noinline)) int V34EchoFilter(void *ctx, short history_offset)
{
    const void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    int estimate = V34EchoFilter__near_original(ctx, history_offset);
    if (bypass && (caller == (const void *)dmodem_near_data_return ||
                   caller == (const void *)dmodem_near_training_return))
        return 0;
    return estimate;
}
