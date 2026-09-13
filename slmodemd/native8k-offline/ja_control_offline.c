/* Offline decoded-Ja event adapter. No socket, modem run loop or RX detector.
 * Uses the same constructor-only fixture as tx_offline.c. */
/* Offline generator ABI test. This executable never starts a modem or opens a
 * network/device. It uses the separately verified gate builder's object set. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
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
extern void tx_phase3(void *) __asm__("_ZN12V90Modulator11enterPhase3Ev");
extern void tx_reset(void *) __asm__("_ZN12V90Modulator5resetEv");
extern void tx_progress(void *, int *, unsigned *, float *, unsigned)
    __asm__("_ZN12V90Modulator8progressEPiRjPfj");
extern void pcm_progress(void *, int *, unsigned *, float *, unsigned)
    __asm__("_ZN8V90Modem8progressEPiRjPfj");
extern short ulaw2linear(unsigned char);
extern unsigned char linear2ulaw(short);
enum { TX_PHASE=0x2c, TX_PHASE_COUNT=0x30, TX_EVENT=0x34,
       TX_PHASE3=0x38, TX_CAPACITY=0x64, TX_SHORT_SAMPLES=0x68,
       PHASE3_STATE=0x14, PHASE3_COUNT=0x18, PHASE3_EVENT=0x1c,
       TOTAL=20000, FRAME=40 };
struct config {uint32_t role,operation,a,b,sample_rate;unsigned char *caps;};
_Static_assert(sizeof(void*)==4, "exact i386 ABI only");
_Static_assert(sizeof(struct config)==24, "V8 config layout");
static int ioctl_stub(struct modem *m,unsigned c,unsigned long a){
    (void)m;(void)a;return c==MDMCTL_IODELAY?512:0;
}
static int abort_io(struct modem *m){(void)m;abort();}
static void arm(void){
    static unsigned char caps[128]; memset(caps,0,sizeof caps);
    caps[0]=0xa8;caps[1]=0x40;
    setenv("DMODEM_V90_DIGITAL_ANSWER","1",1);
    dmodem_v90_host_transition(90,0,8);
    struct config c={V8_ANSWER,0,12,7,9600,caps};
    void *v=V8Create(&c);assert(v);
    const uint16_t cm[]={0x107,0x14d,0x111,0x11,0xa9,0x161,0x1c9,0x11};
    memcpy((char*)v+V8_CM,cm,sizeof cm);
    abi_put16(v,V8_CM+V8_RX_WORD_COUNT,sizeof cm/sizeof *cm);
    rebuildJMSequence(v);int rc=V8UpdateModemParameters(v,caps);
    assert(rc==0 && (caps[0]&V90_CAP));V8Delete(v);
    dmodem_v90_host_transition(90,0,90);
}
#include "ja_frame.h"
extern void DILdescriptorPacker(void *,int16_t *,int16_t *);
/* This boundary accepts already framed/descrambled bits ONLY. The caller must
 * own these live constructor pointers and have verified INFO1a first. It is
 * deliberately not registered as an interposer or a runtime callback. */
static int accept_ja(void *v34,void *pcm,void *tx,const int16_t *b,size_t n)
{
    ja_descriptor d;size_t used=0;
    if(!v34||!pcm||!tx)return JA_INVALID;
    if(abi_ptr(v34,V34_PCM_PTR)!=pcm||abi_ptr(pcm,0)!=v34||
       abi_ptr((char*)pcm+VPCM_V90,0)!=tx||
       abi_ptr((char*)pcm+VPCM_V90,4)||
       abi_ptr(tx,0x0c)!=(char*)pcm+4||!abi_ptr(tx,TX_PHASE3)||
       abi_u32(tx,TX_PHASE)!=0)return JA_INVALID;
    int r=ja_parse(b,n,&d,&used);
    if(r!=JA_COMPLETE)return r;
    /* Copy descriptor bytes, never replace constructor pointers. */
    memcpy((char*)pcm+4,&d,sizeof d);
    tx_phase3(tx);
    return JA_COMPLETE;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);modem_debug_init("ja-control-offline");
    struct modem_driver drv={.name="offline Ja",.start=abort_io,.stop=abort_io,.ioctl=ioctl_stub};
    struct modem *m=modem_create(&drv,"offline");assert(m);
    m->dp_runtime=dp_runtime_create(m);assert(m->dp_runtime);arm();
    struct dp *d=v90_test_vpcm_create(m,90,0,9600,48,NULL);assert(d);
    void *v34=(char*)d+0x2c,*pcm=abi_ptr(v34,V34_PCM_PTR);
    void *tx=abi_ptr((char*)pcm+VPCM_V90,0),*p3=abi_ptr(tx,TX_PHASE3);
    assert(tx&&p3);tx_reset(tx);
    struct {ja_descriptor d;uint8_t pad[16];} input;memset(&input,0,sizeof input);
    input.d.n=3;input.d.lsp=17;input.d.ltp=31;
    for(unsigned i=0;i<17;i++)input.d.sp[i]=i&1;
    for(unsigned i=0;i<31;i++)input.d.tp[i]=(i%3)==0;
    for(unsigned i=0;i<8;i++){input.d.h[i]=i+1;input.d.ref[i]=32+i;}
    for(unsigned i=0;i<3;i++)input.d.ucode[i]=48+i;
    int16_t b[JA_MAX_BITS];int16_t n=0;DILdescriptorPacker(&input.d,b,&n);
    unsigned char old_tx[0x70],old_descriptor[sizeof(ja_descriptor)];
    memcpy(old_tx,tx,sizeof old_tx);memcpy(old_descriptor,(char*)pcm+4,sizeof old_descriptor);
    assert(accept_ja(v34,pcm,tx,b,n-1)==JA_MORE);
    b[60]^=1;assert(accept_ja(v34,pcm,tx,b,n)==JA_INVALID);b[60]^=1;
    assert(!memcmp(old_tx,tx,sizeof old_tx));
    assert(!memcmp(old_descriptor,(char*)pcm+4,sizeof old_descriptor));
    assert(accept_ja(v34,pcm,tx,b,n)==JA_COMPLETE);
    assert(abi_ptr(tx,0x0c)==(char*)pcm+4&&abi_ptr(pcm,0)==v34);
    assert(!memcmp((char*)pcm+4,&input.d,sizeof input.d));
    assert(abi_u32(tx,TX_PHASE)==1&&abi_u32(p3,PHASE3_STATE)==0);
    assert(*((uint8_t*)p3+0x54)==3&&*((uint8_t*)p3+0x55)==17&&*((uint8_t*)p3+0x56)==31);
    assert(!memcmp((char*)p3+0x57,input.d.sp,17));
    assert(!memcmp((char*)p3+0xd7,input.d.tp,31));
    for(unsigned i=0;i<8;i++){
        assert(abi_u32(p3,0x158+4*i)==6U*(input.d.h[i]+1));
        assert((int16_t)abi_u16(p3,0x178+2*i)==ulaw2linear((unsigned char)~input.d.ref[i]));
    }
    for(unsigned i=0;i<3;i++)assert((int16_t)abi_u16(p3,0x188+2*i)==ulaw2linear((unsigned char)~input.d.ucode[i]));
    memcpy(old_tx,tx,sizeof old_tx);
    assert(accept_ja(v34,pcm,tx,b,n)==JA_INVALID&&!memcmp(old_tx,tx,sizeof old_tx));
    struct {uint32_t lo;float s[40];uint32_t hi;} frame={.lo=0x12345678,.hi=0x87654321};
    int bits[1024]={0};unsigned nb=0;tx_progress(tx,bits,&nb,frame.s,40);
    assert(!nb&&frame.lo==0x12345678&&frame.hi==0x87654321);
    unsigned nonzero=0;
    for(unsigned i=0;i<40;i++){assert(isfinite(frame.s[i])&&frame.s[i]==(int16_t)frame.s[i]);if(frame.s[i])nonzero++;}
    assert(nonzero>0);
    printf("PASS decoded-Ja event: %d bits, CRC/truncation rejection leaves TX and descriptor unchanged; accepted descriptor reaches real p3 DIL fields; constructor pointers preserved; duplicate blocked; 40 native symbols (%u nonzero)\n",n,nonzero);
    v90_test_vpcm_delete(d);modem_delete(m);
    puts("OFFLINE ONLY: generated Ja bits, not a received waveform; no INFO1a receiver, Ja detector, S detector, transport, DIL completion or data link");
    return 0;
}
