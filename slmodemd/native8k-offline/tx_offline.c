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
static void collect(struct modem *m, unsigned chunk, int use_common,
                    int16_t out[TOTAL]){
    arm();struct dp *d=v90_test_vpcm_create(m,90,0,9600,48,NULL);assert(d);
    void *v34=(char*)d+0x2c,*pcm=abi_ptr(v34,V34_PCM_PTR);
    void *v90=(char*)pcm+VPCM_V90,*tx=abi_ptr(v90,VPCM_V90_MODULATOR);
    assert(tx && !abi_ptr(v90,VPCM_V90_DEMODULATOR));
    unsigned cap=abi_u32(tx,TX_CAPACITY);assert(chunk<=cap);
    void *p3=abi_ptr(tx,TX_PHASE3);assert(p3);
    printf("constructor chunk=%u common=%d capacity=%u phase=%u law=%u uinfo=%u\n",
           chunk,use_common,cap,abi_u32(tx,TX_PHASE),abi_u32(abi_ptr(tx,0),0),
           *((unsigned char*)abi_ptr(tx,0)+8));
    /* Constructor does not initialize phase/count/event. Allocator reuse
     * exposed a stale phase=1 on the second construction in the first probe. */
    tx_reset(tx);
    assert(abi_u32(tx,TX_PHASE)==0);
    struct {uint32_t lo;float s[FRAME];uint32_t hi;} block;
    int bits[1024];memset(bits,0,sizeof bits);unsigned nb=0;
    block.lo=0x12345678;block.hi=0x87654321;
    tx_progress(tx,bits,&nb,block.s,chunk);
    assert(nb==0 && block.lo==0x12345678 && block.hi==0x87654321);
    for(unsigned i=0;i<chunk;i++)assert(block.s[i]==0);
    tx_phase3(tx);assert(abi_u32(tx,TX_PHASE)==1);
    unsigned last=~0U,nonzero=0,not_exact_ulaw=0;
    for(unsigned n=0;n<TOTAL;n+=chunk){
        unsigned count=TOTAL-n<chunk?TOTAL-n:chunk;
        for(unsigned i=0;i<FRAME;i++)block.s[i]=NAN;
        if(use_common)pcm_progress(v90,bits,&nb,block.s,count);
        else tx_progress(tx,bits,&nb,block.s,count);
        assert(nb==0 && block.lo==0x12345678 && block.hi==0x87654321);
        assert(abi_ptr(pcm,0)==v34);
        const int16_t *vendor=abi_ptr(tx,TX_SHORT_SAMPLES);
        for(unsigned i=0;i<count;i++){
            float f=block.s[i];assert(isfinite(f)&&f>=-32768&&f<=32767&&f==(float)(int16_t)f);
            out[n+i]=(int16_t)f;assert(out[n+i]==vendor[i]);
            if(f!=0)nonzero++;
            if(ulaw2linear(linear2ulaw(out[n+i]))!=out[n+i])not_exact_ulaw++;
        }
        for(unsigned i=count;i<FRAME;i++)assert(isnan(block.s[i]));
        unsigned st=abi_u32(p3,PHASE3_STATE);
        if(st!=last){printf("p3 state=%u after_symbols=%u counter=%u event=%u\n",st,n+count,abi_u32(p3,PHASE3_COUNT),abi_u32(tx,TX_EVENT));last=st;}
    }
    assert(nonzero>0);
    /* With no synthetic peer receive event, the transmitter must wait in Jd;
     * constructor defaults and local generation cannot establish a link. */
    assert(abi_u32(tx,TX_PHASE)==1 && abi_u32(p3,PHASE3_STATE)==3);
    printf("generated=%d nonzero=%u exact_g711_roundtrip_mismatches=%u phase=%u p3=%u\n",TOTAL,nonzero,not_exact_ulaw,abi_u32(tx,TX_PHASE),abi_u32(p3,PHASE3_STATE));
    v90_test_vpcm_delete(d);
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);modem_debug_init("native8k-offline");
    struct modem_driver drv={.name="offline TX",.start=abort_io,.stop=abort_io,.ioctl=ioctl_stub};
    struct modem *m=modem_create(&drv,"offline");assert(m);
    m->dp_runtime=dp_runtime_create(m);assert(m->dp_runtime);
    static int16_t a[TOTAL],b[TOTAL],c[TOTAL];
    collect(m,1,0,a);collect(m,FRAME,0,b);collect(m,FRAME,1,c);
    assert(!memcmp(a,b,sizeof a)&&!memcmp(a,c,sizeof a));
    printf("first24:");for(unsigned i=0;i<24;i++)printf(" %d",a[i]);puts("");
    modem_delete(m);
    puts("OFFLINE TX ABI CHECKS PASS: constructor plus explicit reset, silence, phase3 generation, 1/40 chunk identity, common dispatch, cleanup; no training/call claimed");
}
