/* Experimental V.8/constructor interposers, not a complete PCM data engine.
 * Only the hash-gated builder may link these opaque object offsets.
 * All actual pointers and fragment/mode parameters are preserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include "v90_gate_abi.h"
extern void *V8Create__gate_original(void *);
extern void initTxSequence__gate_original(void *);
extern void rebuildJMSequence__gate_original(void *);
extern int V8UpdateModemParameters__gate_original(void *, unsigned char *);
extern void *VPCMXF_Create__gate_original(int, void *, void *, unsigned, unsigned);
extern int VPcmV34Create__gate_original(void *, int, unsigned, void *, int);
extern void VPCMXF_SessionTermination__gate_original(void *);

static int answer_offer;
static int answer_selected;
static int enabled(void) {
    const char *s = getenv("DMODEM_V90_DIGITAL_ANSWER");
    return s && s[0] == '1' && s[1] == 0;
}
/* Called by the public host immediately before its actual dp create call.
 * V8 cfg does not encode the requested dp90 versus dp92 answer: the vendor
 * clears the V92 capability bit for BOTH answer roles. Do not infer that
 * request from its local PCM capability. One modem runs in each process. */
void dmodem_v90_host_transition(unsigned dp_id, unsigned caller, unsigned engine_id) {
    if (engine_id == 8) { /* DP_V8: a new negotiation */
        answer_selected = 0;
        answer_offer = enabled() && !caller && dp_id == 90;
    } else if (!enabled() || caller || dp_id != 90 || engine_id != 90) {
        answer_selected = answer_offer = 0;
    }
}
static int answer(void *v8) {
    return enabled() && answer_offer && abi_u32(v8, V8_ROLE) == V8_ANSWER;
}
static int find_word(const uint16_t *w, unsigned n, unsigned category) {
    for (unsigned i = 2; i < n; i++)
        if ((w[i] & V8_WORD_CATEGORY_MASK) == category) return (int)i;
    return -1;
}
static int remote_analog(void *v8) {
    const uint16_t *w = (const uint16_t *)((char *)v8 + V8_CM);
    unsigned n = abi_u16(w, V8_RX_WORD_COUNT);
    if (n < 3 || n > 20 || !abi_u16(v8, V8_CALL_MATCH)) return 0;
    /* CM storage excludes the two synchronization words. */
    int data = 0, mod = 0, pcm = 0;
    for (unsigned i = 0; i < n; i++) {
        if (w[i] == V8_WORD_DATA) data = 1;
        if ((w[i] & V8_WORD_CATEGORY_MASK) == V8_WORD_MODULATIONS)
            mod = (w[i] & (V8_WORD_V90 | V8_WORD_V34_DUPLEX)) ==
                  (V8_WORD_V90 | V8_WORD_V34_DUPLEX);
        if ((w[i] & V8_WORD_CATEGORY_MASK) == V8_WORD_PCM)
            pcm = !!(w[i] & V8_WORD_PCM_ANALOG);
    }
    return data && mod && pcm;
}
static int digital_menu(void *v8, int joint) {
    uint16_t *w = abi_ptr(v8, V8_TX);
    unsigned bits = abi_u16(w, V8_TX_BIT_COUNT), n = bits / 10;
    if (bits % 10 || n < 3 || n > V8_TX_MAX_WORDS) return 0;
    int m = find_word(w, n, V8_WORD_MODULATIONS);
    int a = find_word(w, n, V8_WORD_ACCESS);
    int p = find_word(w, n, V8_WORD_PCM);
    if (m < 0 || a < 0 || w[2] != V8_WORD_DATA) return 0;
    if (p < 0 && (!joint || n + 2 > V8_TX_MAX_WORDS)) return 0;
    if (p < 0) {
        p = (int)n;
        w[n++] = V8_WORD_PCM;
        w[n++] = V8_WORD_EXTENSION;
        abi_put16(w, V8_TX_BIT_COUNT, n * 10);
    }
    w[m] |= V8_WORD_V90 | V8_WORD_V34_DUPLEX;
    w[a] |= V8_WORD_DIGITAL_ACCESS;
    w[p] = (w[p] & ~V8_WORD_PCM_ANALOG) | V8_WORD_PCM_DIGITAL;
    fprintf(stderr, "V90_GATE %s words=%u mod=%03x access=%03x pcm=%03x\n",
            joint ? "JM" : "initial", n, w[m], w[a], w[p]);
    return 1;
}
void *V8Create(void *cfg) {
    unsigned char *caps = abi_ptr(cfg, 20);
    int is_answer = abi_u32(cfg, 0) == V8_ANSWER;
    /* A local offer never arms the PCM constructor. Only a completed digital
     * JM selection does. Reset even when a caller reuses this process. */
    answer_selected = 0;
    answer_offer = answer_offer && enabled() && is_answer && (caps[0] & V90_CAP);
    /* The six-byte branch patch retains the vendor dp90/92 filter. The host
     * request gate restores stock no-PCM-offer behavior for unsupported
     * answers, including dp92. Caller V92 behavior remains unchanged. */
    if (is_answer && !answer_offer) caps[0] &= ~V90_CAP;
    void *v8 = V8Create__gate_original(cfg);
    if (!v8) answer_offer = 0;
    return v8;
}
void initTxSequence(void *v8) {
    initTxSequence__gate_original(v8);
    unsigned char *caps = abi_ptr(v8, V8_CAPS);
    if (answer(v8) && (caps[0] & V90_CAP)) digital_menu(v8, 0);
}
void rebuildJMSequence(void *v8) {
    unsigned char *caps = abi_ptr(v8, V8_CAPS);
    int eligible = answer(v8) && (caps[0] & V90_CAP);
    rebuildJMSequence__gate_original(v8);
    if (eligible && remote_analog(v8)) digital_menu(v8, 1);
}
int V8UpdateModemParameters(void *v8, unsigned char *caps) {
    int eligible = answer(v8) && (caps[0] & V90_CAP) && remote_analog(v8);
    answer_selected = 0;
    int result = V8UpdateModemParameters__gate_original(v8, caps);
    if (eligible && result == 0) {
        const uint16_t *w = abi_ptr(v8, V8_TX);
        unsigned bits = abi_u16(w, V8_TX_BIT_COUNT), n = bits / 10;
        int valid = bits % 10 == 0 && n >= 3 && n <= V8_TX_MAX_WORDS;
        int a = valid ? find_word(w, n, V8_WORD_ACCESS) : -1;
        int p = valid ? find_word(w, n, V8_WORD_PCM) : -1;
        int m = valid ? find_word(w, n, V8_WORD_MODULATIONS) : -1;
        if (a >= 0 && p >= 0 && m >= 0 && w[2] == V8_WORD_DATA &&
            (w[m] & (V8_WORD_V90 | V8_WORD_V34_DUPLEX)) ==
                    (V8_WORD_V90 | V8_WORD_V34_DUPLEX) &&
            (w[a] & V8_WORD_DIGITAL_ACCESS) &&
            (w[p] & V8_WORD_PCM_DIGITAL) && !(w[p] & V8_WORD_PCM_ANALOG)) {
            caps[0] |= V90_CAP;
            answer_selected = 1;
            fprintf(stderr, "V90_GATE selected V90 digital-answer remote-analog\n");
        }
    }
    return result;
}
void *VPCMXF_Create(int digital, void *v34, void *params,
                   unsigned milliseconds, unsigned mode) {
    int selected = enabled() && answer_selected ? VPCM_DIGITAL_ARGUMENT : digital;
    void *x = VPCMXF_Create__gate_original(selected, v34, params, milliseconds, mode);
    if (x && enabled() && answer_selected)
        fprintf(stderr, "V90_GATE ctor digital=%d v34_pointer_preserved=%d "
                "modulator=%d demodulator=%d fragment_ms=%u mode=%u\n",
                selected, abi_ptr(x, VPCM_V34_PTR) == v34,
                abi_ptr((char *)x + VPCM_V90, VPCM_V90_MODULATOR) != NULL,
                abi_ptr((char *)x + VPCM_V90, VPCM_V90_DEMODULATOR) != NULL,
                milliseconds, mode);
    return x;
}
int VPcmV34Create(void *v34, int answer_arg, unsigned fragment,
                  void *params, int mode) {
    int selected = answer_arg;
    if (enabled() && answer_selected && mode == VPCM_MODE_V90 && answer_arg == 1)
        selected = 0; /* mode1 inverts this argument at b0b4/b0c8. */
    int result = VPcmV34Create__gate_original(v34, selected, fragment, params, mode);
    if (enabled() && answer_selected)
        fprintf(stderr, "V90_GATE v34 mode=%d arg=%d->%d role=%04x pcm_enabled=%u "
                "fragment=%u params_preserved=1 result=%d\n", mode, answer_arg,
                selected, abi_u16(v34, V34_MODE), abi_u32(v34, V34_PCM_ENABLED),
                fragment, result);
    return result;
}
void VPCMXF_SessionTermination(void *pcm) {
    /* f730 unconditionally dereferences the analog demodulator. The digital
     * constructor has no such object. Destruction still runs normally through
     * VPCMXF_Delete immediately afterward; this skips only analog training
     * history persistence, not deletion. */
    void *v90 = (char *)pcm + VPCM_V90;
    /* Object lifetime outlives negotiation state. A new V8 call or a disabled
     * control must not send an existing digital object through this callback. */
    if (abi_ptr(v90, VPCM_V90_MODULATOR) &&
        !abi_ptr(v90, VPCM_V90_DEMODULATOR)) {
        fprintf(stderr, "V90_GATE digital teardown: no analog demodulator history\n");
        return;
    }
    VPCMXF_SessionTermination__gate_original(pcm);
}
