#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static ssize_t fake_recv(int,void *,size_t,int);
static ssize_t fake_send(int,const void *,size_t,int);
struct sio_test_unused;
#define SIO_RECV fake_recv
#define SIO_SEND fake_send
#define SIO_WAIT(s,fd,events) fake_wait(fd,events)
static int fake_wait(int,short);
#include "dmodem_socket_io.h"

typedef struct {int n,error,cancel;} action;
static const action *read_script,*write_script,*wait_script;
static size_t read_actions,write_actions,wait_actions,rpos,wpos,ppos;
static unsigned char input[8192],output[8192];
static size_t input_len,input_at,output_len,read_chunk,write_chunk;
static size_t reads,writes,waits,last_read_request;
static int cancelled;
static int cancel_test(void){return cancelled;}
static void reset(sio_state *s)
{
    read_script=write_script=wait_script=NULL;
    read_actions=write_actions=wait_actions=rpos=wpos=ppos=0;
    input_len=input_at=output_len=reads=writes=waits=0;
    read_chunk=write_chunk=8192;cancelled=0;
    assert(!sio_init(s,"1",cancel_test));
    for(size_t i=0;i<sizeof input;i++)input[i]=(unsigned char)(i*71+i/3);
}
static int take(const action *script,size_t length,size_t *pos)
{
    assert(*pos<length);action a=script[(*pos)++];
    if(a.cancel)cancelled=1;
    if(a.n<0)errno=a.error;
    return a.n;
}
static ssize_t fake_recv(int fd,void *buf,size_t n,int flags)
{
    assert(fd==7&&flags==MSG_DONTWAIT);++reads;last_read_request=n;
    int result=read_script?take(read_script,read_actions,&rpos):(int)read_chunk;
    if(result<=0)return result;
    size_t got=(size_t)result;
    if(got>n)got=n;
    if(got>input_len-input_at)got=input_len-input_at;
    memcpy(buf,input+input_at,got);input_at+=got;return (ssize_t)got;
}
static ssize_t fake_send(int fd,const void *buf,size_t n,int flags)
{
    assert(fd==7&&flags==(MSG_DONTWAIT|MSG_NOSIGNAL));++writes;
    int result=write_script?take(write_script,write_actions,&wpos):(int)write_chunk;
    if(result<=0)return result;
    size_t sent=(size_t)result;if(sent>n)sent=n;
    assert(output_len+sent<=sizeof output);
    memcpy(output+output_len,buf,sent);output_len+=sent;return (ssize_t)sent;
}
static int fake_wait(int fd,short events)
{
    assert(fd==7&&(events==POLLIN||events==POLLOUT));++waits;
    return wait_script?take(wait_script,wait_actions,&ppos):1;
}
static void read_boundaries(void)
{
    sio_state s;unsigned char gathered[8192];
    for(size_t chunk=1;chunk<=4096;chunk++){
        reset(&s);input_len=1536;read_chunk=chunk;size_t used=0;
        for(;;){
            struct {uint64_t a;char b[384];uint64_t z;} frame;
            frame.a=0x123456789abcdef0ULL;frame.z=~frame.a;
            int n=sio_read(&s,7,frame.b,192);
            assert(frame.a==0x123456789abcdef0ULL&&frame.z==~frame.a);
            assert(n>=0&&n<=192);if(!n)break;
            memcpy(gathered+used,frame.b,(size_t)n*2);used+=(size_t)n*2;
        }
        assert(used==input_len&&!memcmp(gathered,input,input_len));
        assert(s.rx_bytes==input_len&&!s.pending&&!s.io_errors&&!s.instrument_errors);
    }
    reset(&s);input_len=4096;char big[4096];
    assert(sio_read(&s,7,big,2048)==2048&&reads==1&&last_read_request==4096);
    reset(&s);input_len=384;
    assert(sio_read(&s,7,big,2048)==192&&reads==1&&last_read_request==4096);
    puts("PASS read fragmentation 1..4096 bytes; exact stream, canaries, unchanged 384/4096-byte ready chunks");
}
static void faults(void)
{
    sio_state s;char b[32];
    reset(&s);input_len=4;
    const action r[]={{-1,EINTR,0},{3,0,0},{-1,EAGAIN,0},{1,0,0}};
    read_script=r;read_actions=4;
    assert(sio_read(&s,7,b,16)==1&&s.pending);
    assert(!memcmp(b,input,2));
    assert(sio_read(&s,7,b,16)==1&&!s.pending&&!memcmp(b,input+2,2));
    assert(s.read_eintr==1&&s.read_waits==1&&waits==1&&s.odd_reads==2);
    reset(&s);input_len=1;read_chunk=1;
    assert(sio_read(&s,7,b,16)==-1&&errno==EPROTO&&s.truncated_samples==1);
    reset(&s);input_len=3;read_chunk=3;
    assert(sio_read(&s,7,b,16)==1&&s.pending);
    assert(sio_read(&s,7,b,16)==-1&&errno==EPROTO);
    assert(!sio_init(&s,"1",cancel_test)&&!s.pending&&!s.rx_bytes);
    reset(&s);const action ce[]={{1,0,0},{-1,EINTR,1}};
    input_len=2;read_script=ce;read_actions=2;
    assert(sio_read(&s,7,b,16)==-1&&errno==ECANCELED&&s.rx_bytes==1&&s.cancelled_io==1);
    reset(&s);const action fatal[]={{-1,ECONNRESET,0}};
    read_script=fatal;read_actions=1;
    assert(sio_read(&s,7,b,16)==-1&&errno==ECONNRESET&&s.io_errors==1);
    reset(&s);const action again[]={{-1,EAGAIN,0}};
    const action badwait[]={{-1,EBADF,0}};
    read_script=again;read_actions=1;wait_script=badwait;wait_actions=1;
    assert(sio_read(&s,7,b,16)==-1&&errno==EBADF&&reads==1&&waits==1);
    puts("PASS read EINTR/EAGAIN, exact fatal errno, odd EOF, call reset and cancellation");
}
static void write_boundaries(void)
{
    sio_state s;
    for(size_t chunk=1;chunk<=384;chunk++){
        reset(&s);write_chunk=chunk;
        assert(sio_write(&s,7,(char*)input,192)==192);
        assert(output_len==384&&!memcmp(output,input,384)&&s.tx_bytes==384);
    }
    reset(&s);
    const action w[]={{1,0,0},{-1,EINTR,0},{3,0,0},{-1,EAGAIN,0},{380,0,0}};
    write_script=w;write_actions=5;
    assert(sio_write(&s,7,(char*)input,192)==192&&!memcmp(input,output,384));
    assert(s.write_eintr==1&&s.write_waits==1&&s.short_writes==2&&waits==1);
    reset(&s);const action zero[]={{2,0,0},{0,0,0}};write_script=zero;write_actions=2;
    assert(sio_write(&s,7,(char*)input,192)==-1&&errno==EIO&&output_len==2&&writes==2);
    reset(&s);const action fail[]={{3,0,0},{-1,EPIPE,0}};write_script=fail;write_actions=2;
    assert(sio_write(&s,7,(char*)input,192)==-1&&errno==EPIPE&&output_len==3&&s.tx_bytes==3);
    reset(&s);const action ce[]={{3,0,0},{-1,EINTR,1}};write_script=ce;write_actions=2;
    assert(sio_write(&s,7,(char*)input,192)==-1&&errno==ECANCELED&&output_len==3);
    reset(&s);cancelled=1;
    assert(sio_write(&s,7,(char*)input,192)==-1&&errno==ECANCELED&&!writes);
    puts("PASS every 1..384-byte short write, suffix identity, EINTR/EAGAIN, zero progress, EPIPE and cancellation");
}
static void delay_bounds(void)
{
    sio_state s;reset(&s);int16_t b[192];void *p=b;
    int count=192,adjust=-48,delay=192;
    assert(!sio_skip(&s,&p,&count,&adjust,&delay));
    assert(p==b+48&&count==144&&adjust==0&&delay==144&&s.skipped_samples==48);
    p=b;count=192;adjust=-250;delay=500;
    assert(!sio_skip(&s,&p,&count,&adjust,&delay)&&count==0&&adjust==-58&&delay==308);
    p=b;count=192;
    assert(!sio_skip(&s,&p,&count,&adjust,&delay)&&p==b+58&&count==134&&adjust==0&&delay==250);
    p=b;count=192;adjust=INT_MIN;delay=192;
    assert(!sio_skip(&s,&p,&count,&adjust,&delay)&&count==0&&adjust==INT_MIN+192&&delay==0);
    p=b;count=192;adjust=-1;delay=INT_MIN;
    assert(sio_skip(&s,&p,&count,&adjust,&delay)==-1&&errno==EINVAL);
    assert(p==b&&count==192&&adjust==-1&&delay==INT_MIN);
    assert(!sio_padding(&s,2048,4096,0));
    assert(sio_padding(&s,2049,4096,0)==-1&&errno==EINVAL);
    assert(sio_padding(&s,INT_MAX,4096,0)==-1);
    assert(sio_padding(&s,1,4096,INT_MAX)==-1);
    assert(sio_padding(&s,-1,4096,0)==-1);
    assert(!reads&&!writes);
    puts("PASS partial/full/carried/INT_MIN skips and atomic delay errors; padding bounds before I/O");
}
int main(void)
{
    sio_state s;const char *bad[]={"","2","01"," 1","-1","true"};
    assert(!sio_init(&s,NULL,NULL)&&!s.enabled);
    assert(!sio_init(&s,"0",NULL)&&!s.enabled);
    for(size_t i=0;i<sizeof bad/sizeof bad[0];i++)
        assert(sio_init(&s,bad[i],NULL)==-1&&errno==EINVAL&&s.instrument_errors==1&&!s.enabled);
    read_boundaries();faults();write_boundaries();delay_bounds();
    reset(&s);sio_report(&s);
    puts("SOCKET IO UNIT CHECKS PASS");return 0;
}
