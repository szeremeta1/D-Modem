/* Exact checked i386 DSP ABI only; see README.md and the hash-gated builder. */
#ifndef V90_GATE_ABI_H
#define V90_GATE_ABI_H
#include <stdint.h>
#include <string.h>
enum {
    V8_ROLE = 0xa44, V8_CAPS = 0xa58, V8_TX = 0xc48,
    V8_CM = 0xc54, V8_RX_WORD_COUNT = 0x28,
    V8_TX_BIT_COUNT = 0x22, V8_TX_MAX_WORDS = 15,
    V8_CALL_MATCH = 0xebc,
    V8_ANSWER = 1,
    V8_WORD_CATEGORY_MASK = 0xfff1,
    V8_WORD_DATA = 0x107, V8_WORD_MODULATIONS = 0x141,
    V8_WORD_ACCESS = 0x161, V8_WORD_PCM = 0x1c1,
    V8_WORD_EXTENSION = 0x11,
    V8_WORD_V90 = 0x08, V8_WORD_V34_DUPLEX = 0x04,
    V8_WORD_DIGITAL_ACCESS = 0x02,
    V8_WORD_PCM_ANALOG = 0x08, V8_WORD_PCM_DIGITAL = 0x04,
    V90_CAP = 0x08,
    VPCM_MODE_V90 = 1, VPCM_ANALOG_ARGUMENT = 0,
    VPCM_DIGITAL_ARGUMENT = 1,
    VPCM_V34_PTR = 0, VPCM_V90 = 0x1758,
    VPCM_V90_MODULATOR = 0, VPCM_V90_DEMODULATOR = 4,
    V34_PCM_PTR = 0x3548, V34_MODE = 0x359c,
    V34_ANALOG_MODE = 0x65, V34_DIGITAL_MODE = 0x66,
    V34_PCM_ENABLED = 0x24c
};
static inline uint16_t abi_u16(const void *p, unsigned off) {
    uint16_t v; memcpy(&v, (const char *)p + off, sizeof v); return v;
}
static inline uint32_t abi_u32(const void *p, unsigned off) {
    uint32_t v; memcpy(&v, (const char *)p + off, sizeof v); return v;
}
static inline void *abi_ptr(const void *p, unsigned off) {
    return (void *)(uintptr_t)abi_u32(p, off);
}
static inline void abi_put16(void *p, unsigned off, uint16_t v) {
    memcpy((char *)p + off, &v, sizeof v);
}
#endif
