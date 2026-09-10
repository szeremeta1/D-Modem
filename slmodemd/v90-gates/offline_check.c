/* Executes actual vendor constructors and menu builders with aborting line-I/O
 * stubs. No waveform handshake, network call, or negotiated-rate claim. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem.h"
#include "modem_dp.h"
#include "modem_debug.h"
#include "v90_gate_abi.h"
extern void dmodem_v90_host_transition(unsigned, unsigned, unsigned);
extern void *V8Create(void *);
extern void V8Delete(void *);
extern void rebuildJMSequence(void *);
extern int V8UpdateModemParameters(void *, unsigned char *);
extern void *dp_runtime_create(struct modem *);
extern struct dp *v90_test_vpcm_create(struct modem *, int, int, int, int, void *);
extern int v90_test_vpcm_delete(struct dp *);
extern void V34SetINFO0dBits(void *, short *);
struct config { uint32_t role, operation, a, b, sample_rate; unsigned char *caps; };
_Static_assert(sizeof(void *) == 4, "32-bit exact vendor ABI required");
_Static_assert(sizeof(struct config) == 24, "V8 config ABI");
static int ioctl_stub(struct modem *m, unsigned cmd, unsigned long arg) {
    (void)m; (void)arg; return cmd == MDMCTL_IODELAY ? 512 : 0;
}
static int start_stop_stub(struct modem *m) { (void)m; abort(); }
static int word_at(void *v8, unsigned category) {
    uint16_t *words = abi_ptr(v8, V8_TX);
    unsigned n = abi_u16(words, V8_TX_BIT_COUNT) / 10;
    assert(n <= V8_TX_MAX_WORDS);
    for (unsigned i = 2; i < n; i++)
        if ((words[i] & V8_WORD_CATEGORY_MASK) == category) return words[i];
    return -1;
}
static void init_caps(unsigned char *caps) {
    memset(caps, 0, 128); caps[0] = 0xa8; caps[1] = 0x40;
}
static void *v8_case(unsigned char *caps, int active, int requested_dp, int peer_pcm) {
    setenv("DMODEM_V90_DIGITAL_ANSWER", active ? "1" : "0", 1);
    dmodem_v90_host_transition(requested_dp, 0, 8);
    init_caps(caps);
    struct config cfg = { V8_ANSWER, 0, 12, 7, 9600, caps };
    void *v8 = V8Create(&cfg); assert(v8);
    const uint16_t offered[] = {0x107, 0x14d, 0x111, 0x11, 0xa9, 0x161, 0x1c9, 0x11};
    uint16_t cm[sizeof offered / sizeof offered[0]];
    memcpy(cm, offered, sizeof cm);
    if (!peer_pcm) cm[6] = V8_WORD_PCM;
    memcpy((char *)v8 + V8_CM, cm, sizeof cm);
    abi_put16(v8, V8_CM + V8_RX_WORD_COUNT, sizeof cm / sizeof cm[0]);
    rebuildJMSequence(v8);
    int access = word_at(v8, V8_WORD_ACCESS), pcm = word_at(v8, V8_WORD_PCM);
    assert(access >= 0);
    int selected = V8UpdateModemParameters(v8, caps);
    assert(!memcmp((char *)v8 + V8_CM, cm, sizeof cm));
    printf("V8 active=%d requested_dp=%d peer_analog_pcm=%d access=%03x pcm=%03x v90=%d result=%d\n",
           active, requested_dp, peer_pcm, access, pcm, !!(caps[0] & V90_CAP), selected);
    if (active && requested_dp == 90 && peer_pcm) {
        assert(access & V8_WORD_DIGITAL_ACCESS);
        assert(pcm == (V8_WORD_PCM | V8_WORD_PCM_DIGITAL));
        assert((caps[0] & V90_CAP) && selected == 0);
    } else assert(!(caps[0] & V90_CAP));
    return v8;
}
static struct dp *pcm_case(struct modem *m, int dp_id, int digital, const char *label) {
    dmodem_v90_host_transition(dp_id, 0, dp_id);
    struct dp *dp = v90_test_vpcm_create(m, dp_id, 0, 9600, 48, NULL); assert(dp);
    void *v34 = (char *)dp + 0x2c;
    void *pcm = abi_ptr(v34, V34_PCM_PTR); assert(pcm);
    assert(abi_ptr(pcm, VPCM_V34_PTR) == v34);
    int mod = abi_ptr((char *)pcm + VPCM_V90, VPCM_V90_MODULATOR) != NULL;
    int dem = abi_ptr((char *)pcm + VPCM_V90, VPCM_V90_DEMODULATOR) != NULL;
    printf("PCM %s dp=%d modulator=%d demodulator=%d role=%04x pcm_enabled=%u\n",
           label, dp_id, mod, dem, abi_u16(v34, V34_MODE), abi_u32(v34, V34_PCM_ENABLED));
    assert(mod == digital && dem == !digital);
    if (dp_id == 90) {
        assert(abi_u16(v34, V34_MODE) == (digital ? V34_DIGITAL_MODE : V34_ANALOG_MODE));
        assert(abi_u32(v34, V34_PCM_ENABLED) == 1);
    }
    return dp;
}
static void caller92_control(void) {
    unsigned char caps[128], reference_caps[128];
    uint16_t reference_words[V8_TX_MAX_WORDS];
    unsigned reference_n = 0;
    for (int active = 0; active <= 1; active++) {
        setenv("DMODEM_V90_DIGITAL_ANSWER", active ? "1" : "0", 1);
        dmodem_v90_host_transition(92, 1, 8);
        init_caps(caps); caps[2] = 0x10;
        struct config cfg = {0, 0, 12, 7, 9600, caps};
        void *v8 = V8Create(&cfg); assert(v8);
        const uint16_t *words = abi_ptr(v8, V8_TX);
        unsigned n = abi_u16(words, V8_TX_BIT_COUNT) / 10;
        assert(n <= V8_TX_MAX_WORDS);
        if (!active) {
            memcpy(reference_caps, caps, sizeof caps);
            memcpy(reference_words, words, n * sizeof *words); reference_n = n;
        } else {
            assert(n == reference_n && !memcmp(reference_caps, caps, sizeof caps));
            assert(!memcmp(reference_words, words, n * sizeof *words));
        }
        V8Delete(v8);
    }
    puts("V92 caller initial capabilities/menu: enabled and disabled controls identical");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    modem_debug_init("v90-gate-offline");
    struct modem_driver driver = { .name="offline gates", .start=start_stop_stub,
        .stop=start_stop_stub, .ioctl=ioctl_stub };
    struct modem *m = modem_create(&driver, "offline"); assert(m);
    m->dp_runtime = dp_runtime_create(m); assert(m->dp_runtime);
    unsigned char caps[128];
    void *v8 = v8_case(caps, 0, 90, 1);
    v90_test_vpcm_delete(pcm_case(m, 90, 0, "disabled")); V8Delete(v8);
    v8 = v8_case(caps, 1, 90, 0);
    v90_test_vpcm_delete(pcm_case(m, 90, 0, "rejected-PCM-does-not-arm-constructor"));
    v90_test_vpcm_delete(pcm_case(m, 34, 0, "actual-V34-fallback")); V8Delete(v8);
    v8 = v8_case(caps, 1, 90, 1);
    struct dp *dp = pcm_case(m, 90, 1, "selected-digital");
    void *v34 = (char *)dp + 0x2c;
    short info[32]; memset(info, 0x7f, sizeof info);
    V34SetINFO0dBits(v34, info);
    assert((uint16_t)info[12] == 0x001e);
    puts("Digital INFO0 marker word = 001e; original V34 pointer preserved");
    /* Cleanup follows the actual object, even after the control is disabled. */
    setenv("DMODEM_V90_DIGITAL_ANSWER", "0", 1);
    dmodem_v90_host_transition(90, 0, 8);
    v90_test_vpcm_delete(dp); V8Delete(v8);
    puts("Digital cleanup survives control disable and new-call state reset");
    /* A new V8 object must independently clear an earlier successful choice. */
    v8 = v8_case(caps, 1, 90, 1); V8Delete(v8);
    init_caps(caps);
    struct config cfg = {V8_ANSWER, 0, 12, 7, 9600, caps};
    v8 = V8Create(&cfg); assert(v8);
    v90_test_vpcm_delete(pcm_case(m, 90, 0, "new-V8-before-selection")); V8Delete(v8);
    /* An update that rejects the peer invalidates a previous success too. */
    v8 = v8_case(caps, 1, 90, 1);
    abi_put16(v8, V8_CM + 12, V8_WORD_PCM);
    V8UpdateModemParameters(v8, caps);
    v90_test_vpcm_delete(pcm_case(m, 90, 0, "selection-rejected-on-later-update")); V8Delete(v8);
    /* A syntactically invalid emitted JM cannot arm a constructor. */
    v8 = v8_case(caps, 1, 90, 1);
    abi_put16(abi_ptr(v8, V8_TX), V8_TX_BIT_COUNT, 39);
    V8UpdateModemParameters(v8, caps);
    v90_test_vpcm_delete(pcm_case(m, 90, 0, "invalid-JM-bit-count")); V8Delete(v8);
    v8 = v8_case(caps, 1, 90, 1);
    v90_test_vpcm_delete(pcm_case(m, 34, 0, "host-transition-to-V34-clears-selection")); V8Delete(v8);
    v8 = v8_case(caps, 1, 92, 1);
    v90_test_vpcm_delete(pcm_case(m, 92, 0, "unsupported-V92-answer")); V8Delete(v8);
    caller92_control();
    modem_delete(m);
    puts("OFFLINE GATE CHECKS PASS; no call or training was performed");
    return 0;
}
