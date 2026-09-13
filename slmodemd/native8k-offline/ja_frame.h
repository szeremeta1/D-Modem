/* Offline candidate: parse a complete, already descrambled V.90 Ja frame.
 * Framing, field layout and CRC retained from the earlier packed-bit probe;
 * validation against the pinned public DSP is still required.
 * This is not an RF/PCM receiver and does not select a modem state. */
#ifndef DMODEM_JA_FRAME_H
#define DMODEM_JA_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    uint8_t n, lsp, ltp;
    uint8_t sp[128], tp[128], h[8], ref[8], ucode[255];
} ja_descriptor;
enum { JA_INVALID=-1, JA_MORE=0, JA_COMPLETE=1, JA_MAX_BITS=2654 };
static unsigned ja_field(const int16_t *b, size_t pos, unsigned width)
{
    unsigned v=0, i;
    for(i=0;i<width;i++)v|=(unsigned)b[pos+i]<<i;
    return v;
}
/* MSB-first 16-stage register; wire payload bits themselves arrive in order.
 * Start bits and frame sync are excluded. The reviewed packer layout is at
 * 0x4da80..0x4dd9b; public-object execution remains a separate validation. */
static uint16_t ja_crc(const int16_t *b, size_t crc_start)
{
    uint16_t crc=0xffff;
    size_t pos;
    for(pos=18;pos<crc_start;pos++) {
        unsigned feedback;
        if((pos-17)%17==0)continue;
        feedback=((crc>>15)&1)^(unsigned)b[pos];
        crc=(uint16_t)(crc<<1);
        if(feedback)crc^=0x1021;
    }
    return crc;
}
/* Consume one frame starting with its 17-one sync. Output is committed only
 * after framing/length/CRC pass. Reserved payload bits are CRC-covered but
 * deliberately not interpreted. Extra bits after consumed belong to caller. */
static int ja_parse(const int16_t *bits,size_t count,ja_descriptor *out,size_t *consumed)
{
    ja_descriptor tmp;
    size_t i,alpha,beta,crc_start,total,base;
    unsigned actual_crc=0;
    if(!bits||!out||!consumed)return JA_INVALID;
    *consumed=0;
    for(i=0;i<count&&i<52;i++)if(bits[i]!=0&&bits[i]!=1)return JA_INVALID;
    if(count<52)return JA_MORE;
    for(i=0;i<17;i++)if(bits[i]!=1)return JA_INVALID;
    if(bits[17]||bits[34]||bits[51])return JA_INVALID;
    memset(&tmp,0,sizeof(tmp));
    tmp.n=(uint8_t)ja_field(bits,18,8);
    tmp.lsp=(uint8_t)(ja_field(bits,35,7)+1);
    tmp.ltp=(uint8_t)(ja_field(bits,43,7)+1);
    alpha=((tmp.lsp+15)/16)*17;
    beta=alpha+((tmp.ltp+15)/16)*17;
    crc_start=187+beta+((tmp.n+1)/2)*17;
    total=crc_start+18;
    if(total&1)total++;
    if(total>JA_MAX_BITS)return JA_INVALID;
    for(i=52;i<count&&i<total;i++)if(bits[i]!=0&&bits[i]!=1)return JA_INVALID;
    if(count<total)return JA_MORE;
    for(i=17;i<=crc_start;i+=17)if(bits[i])return JA_INVALID;
    for(i=0;i<16;i++)actual_crc=(actual_crc<<1)|(unsigned)bits[crc_start+1+i];
    if(actual_crc!=ja_crc(bits,crc_start))return JA_INVALID;
    for(i=crc_start+17;i<total;i++)if(bits[i])return JA_INVALID;
    for(i=0;i<tmp.lsp;i++)tmp.sp[i]=(uint8_t)bits[52+i+i/16];
    for(i=0;i<tmp.ltp;i++)tmp.tp[i]=(uint8_t)bits[52+alpha+i+i/16];
    base=52+beta;
    for(i=0;i<8;i++)tmp.h[i]=(uint8_t)ja_field(bits,base+8*i+i/2,7);
    base=120+beta;
    for(i=0;i<8;i++)tmp.ref[i]=(uint8_t)ja_field(bits,base+8*i+i/2,7);
    base=188+beta;
    for(i=0;i<tmp.n;i++)tmp.ucode[i]=(uint8_t)ja_field(bits,base+8*i+i/2,7);
    *out=tmp;*consumed=total;return JA_COMPLETE;
}
#endif
