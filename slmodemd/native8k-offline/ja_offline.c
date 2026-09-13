/* Real vendor packer -> independent bounded parser. No modem is constructed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ja_frame.h"
extern void DILdescriptorPacker(void *,int16_t *,int16_t *);
_Static_assert(sizeof(void*)==4,"pinned i386 ABI required");
_Static_assert(sizeof(ja_descriptor)==0x212,"verified descriptor byte layout");
static void one(unsigned n,unsigned sp,unsigned tp,int exhaustive)
{
    /* Vendor packer reads the second Ucode of an odd pair; reserve and zero
     * padding beyond the 255-byte payload. Never pass a minimal-sized object. */
    struct {ja_descriptor d;uint8_t pad[16];} input;
    struct {uint32_t lo;int16_t b[JA_MAX_BITS+16];uint32_t hi;} wire;
    ja_descriptor got,unchanged;
    int16_t length=-1;
    size_t used=123,i;
    memset(&input,0,sizeof(input));
    input.d.n=n;input.d.lsp=sp;input.d.ltp=tp;
    for(i=0;i<sp;i++)input.d.sp[i]=(i%3)==0;
    for(i=0;i<tp;i++)input.d.tp[i]=(i%5)<2;
    for(i=0;i<8;i++){input.d.h[i]=(17*i+7)%128;input.d.ref[i]=(16*i+5)%128;}
    for(i=0;i<n;i++)input.d.ucode[i]=(29*i+12)%128;
    for(i=0;i<JA_MAX_BITS+16;i++)wire.b[i]=1234;
    wire.lo=0x12345678;wire.hi=0x87654321;
    DILdescriptorPacker(&input.d,wire.b,&length);
    assert(wire.lo==0x12345678&&wire.hi==0x87654321);
    assert(length>0&&length<=JA_MAX_BITS);
    for(i=(size_t)length;i<JA_MAX_BITS+16;i++)assert(wire.b[i]==1234);
    memset(&got,0xa5,sizeof(got));unchanged=got;
    int status=ja_parse(wire.b,length,&got,&used);
    if(status!=JA_COMPLETE) {
        size_t a=((sp+15)/16)*17,beta=a+((tp+15)/16)*17,c=187+beta+((n+1)/2)*17;
        unsigned wc=0;for(i=0;i<16;i++)wc=(wc<<1)|wire.b[c+1+i];
        printf("FAIL N=%u sp=%u tp=%u len=%d crc_start=%zu wire_crc=%04x local_crc=%04x status=%d\n",n,sp,tp,length,c,wc,ja_crc(wire.b,c),status);
    }
    assert(status==JA_COMPLETE&&used==(size_t)length);
    assert(!memcmp(&got,&input.d,sizeof(got)));
    /* A complete first frame does not inspect unrelated trailing values. */
    assert(ja_parse(wire.b,length+16,&got,&used)==JA_COMPLETE);
    assert(used==(size_t)length&&!memcmp(&got,&input.d,sizeof(got)));
    if(!exhaustive)return;
    unsigned corruptions=0;
    /* Every protected payload/CRC bit and each start/sync/fill bit rejects.
     * Reserved fields still carry CRC protection. Length bits may instead
     * require more input; neither result may commit the output descriptor. */
    for(i=0;i<(size_t)length;i++){
        wire.b[i]^=1;got=unchanged;used=123;
        status=ja_parse(wire.b,length,&got,&used);
        assert(status!=JA_COMPLETE&&!memcmp(&got,&unchanged,sizeof(got))&&used==0);
        wire.b[i]^=1;corruptions++;
    }
    for(i=0;i<(size_t)length;i++){
        got=unchanged;used=123;
        assert(ja_parse(wire.b,i,&got,&used)==JA_MORE);
        assert(!memcmp(&got,&unchanged,sizeof(got))&&used==0);
    }
    printf("PASS N=%u LSP=%u LTP=%u bits=%d bad_single_bits=%u all_truncations=MORE\n",n,sp,tp,length,corruptions);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    one(0,1,1,1);one(1,1,1,1);one(3,17,31,1);one(128,128,128,1);one(255,128,128,1);
    const unsigned lengths[]={1,15,16,17,31,32,127,128};
    for(unsigned n=0;n<=255;n++)
        for(unsigned s=0;s<8;s++)
            for(unsigned t=0;t<8;t++)one(n,lengths[s],lengths[t],0);
    puts("PASS 16384 descriptor boundary combinations, including every N 0..255; trailing values ignored after consumed frame");
    puts("JA FRAME CHECKS PASS: vendor-packed descriptor roundtrip, every single-bit corruption rejected, every truncation held; no received waveform or modem state claim");
}
