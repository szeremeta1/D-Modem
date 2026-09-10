/* Direct real-vendor function checks with synthetic in-memory state only.
 * No device, network, waveform replay, complete datapump, or call is started.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void V34InitializeImplementationSpecific(void *);
extern void V34EchoCleanUp(void *);
extern int V34EchoFilter(void *, short);
extern int V34EchoFilter__near_original(void *, short);
extern void V34EchoAdapt(void *, short);
extern int modem_serrint(void *);
extern int adaptecho(void *);
struct echo {
    int16_t *write, *base, *coeff, *fraction, *working;
    uint32_t state, ring_length, taps;
};
_Static_assert(sizeof(void *) == 4 && sizeof(struct echo) == 32, "vendor ABI");
_Static_assert(offsetof(struct echo, working) == 0x10, "history offset");
_Static_assert(offsetof(struct echo, taps) == 0x1c, "tap count offset");
enum { NEAR = 0x80b8, FAR = 0x9138, OBJECT_SIZE = 0xb000 };
static void put16(void *p, size_t n, uint16_t v) { memcpy((char *)p+n,&v,2); }
static int16_t get16(void *p, size_t n) { int16_t v;memcpy(&v,(char *)p+n,2);return v; }
static void putptr(void *p, size_t n, void *v) { memcpy((char *)p+n,&v,4); }
static struct echo *ec(void *p, size_t n) { return (void *)((char *)p+n); }
static void *fixture(void)
{
    char *p = calloc(1, OBJECT_SIZE); assert(p);
    V34InitializeImplementationSpecific(p);
    for (unsigned j=0;j<2;j++) {
        struct echo *e=ec(p,j?FAR:NEAR);
        V34EchoCleanUp(e);
        assert(e->taps==144 && e->ring_length==1656 && e->write==e->base);
        for (unsigned i=0;i<e->ring_length;i++) e->base[i]=(i%200)*3+100;
        for (unsigned i=0;i<e->taps;i++) e->coeff[i]=j?4096:8192;
    }
    putptr(p,0x2220,p+0x2228); /* TX queue cursor consumed by both callers */
    putptr(p,0x026c,p+0x0270); /* receive output cursor */
    put16(p,0x0260,1000);       /* current RX sample */
    put16(p,0x25c2,0x0204);     /* filter active; adaptation frozen */
    put16(p,0x0386,0x8000);     /* real caller's direct real-sample output branch */
    return p;
}
static int rounded(int n) { return (int32_t)(0x8000+4*n)>>16; }
static void history_equal(struct echo *a,struct echo *b)
{
    assert(a->write-a->base==b->write-b->base);
    assert(memcmp(a->base,b->base,a->ring_length*2)==0);
    assert(memcmp(a->working,b->working,a->taps*2)==0);
    assert(memcmp(a->coeff,b->coeff,a->taps*2)==0);
    assert(memcmp(a->fraction,b->fraction,a->taps*2)==0);
}
static void data_case(int active,int far_enabled)
{
    void *actual=fixture(),*reference=fixture();
    put16(actual,0xa23c,far_enabled);
    int near=V34EchoFilter__near_original(ec(reference,NEAR),0);
    int far=far_enabled?V34EchoFilter__near_original(ec(reference,FAR),0):0;
    assert(near && (!far_enabled || far));
    assert(modem_serrint(actual)==0);
    int expected=1000+rounded((active?0:near)+far);
    assert(get16(actual,0x0270)==expected);
    assert(get16(actual,0x2f58)==expected);
    history_equal(ec(actual,NEAR),ec(reference,NEAR));
    history_equal(ec(actual,FAR),ec(reference,FAR));
    printf("real modem_serrint near_bypass=%d far_enabled=%d RX=%d expected=%d; both histories preserved\n",
           active,far_enabled,get16(actual,0x0270),expected);
    free(actual);free(reference);
}
static void training_case(int active,int frozen)
{
    void *actual=fixture(),*reference=fixture();
    if (!frozen) {
        put16(actual,0x25c2,0x0200);
        put16(actual,0x3550,(uint16_t)-4096);
        uint32_t gain=1;memcpy((char *)actual+0x355c,&gain,4);
    }
    int estimate=V34EchoFilter__near_original(ec(reference,NEAR),0);assert(estimate);
    assert(adaptecho(actual)==0);
    int expected=1000+rounded(active?0:estimate);
    assert(get16(actual,0x0260)==expected);
    if (!frozen) {
        short error=(short)((-4096*expected+0x2000)>>14);
        V34EchoAdapt(ec(reference,NEAR),error);
        assert(ec(actual,NEAR)->coeff[143]!=8192);
    }
    history_equal(ec(actual,NEAR),ec(reference,NEAR));
    history_equal(ec(actual,FAR),ec(reference,FAR));
    printf("real adaptecho near_bypass=%d frozen=%d RX=%d expected=%d; near history and adaptation checked, far untouched\n",
           active,frozen,get16(actual,0x0260),expected);
    free(actual);free(reference);
}
int main(int argc,char **argv)
{
    assert(argc==2);int active=atoi(argv[1]);assert(active==0||active==1);
    setvbuf(stdout,NULL,_IONBF,0);
    /* Unknown callers always preserve the actual vendor result, even active.
     * These offsets also test signed-16 ABI and history-ring wrap behavior. */
    const short offsets[]={-5,0,3,1514};
    for(unsigned j=0;j<2;j++) for(unsigned i=0;i<4;i++) {
        void *a=fixture(),*b=fixture();struct echo *ea=ec(a,j?FAR:NEAR),*eb=ec(b,j?FAR:NEAR);
        int x=V34EchoFilter(ea,offsets[i]);
        int y=V34EchoFilter__near_original(eb,offsets[i]);
        assert(x==y && y!=0);history_equal(ea,eb);free(a);free(b);
    }
    puts("unknown-caller pass-through: both contexts, signed offsets and ring wrap PASS");
    data_case(active,0);data_case(active,1);training_case(active,1);training_case(active,0);
    puts("OFFLINE NEAR EC CHECKS PASS; no call, replay, or training was performed");
    return 0;
}
