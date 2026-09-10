/* Real AF_UNIX socketpair test, no external network/modem/device. */
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "dmodem_socket_io.h"
static volatile sig_atomic_t cancelled,pipe_signals;
static int cancel_test(void){return cancelled;}
static void term(int sig){(void)sig;cancelled=1;}
static void pipe_seen(int sig){(void)sig;++pipe_signals;}
static void blocked(int writer)
{
    int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    int before=fcntl(fd[0],F_GETFL);assert(before>=0);
    char b[4096]={0};
    if(writer){
        int cap=4096;assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&cap,sizeof cap));
        while(send(fd[0],b,sizeof b,MSG_DONTWAIT|MSG_NOSIGNAL)>0){}
        assert(errno==EAGAIN||errno==EWOULDBLOCK);
    }
    sigset_t before_mask,after_mask;
    assert(!sigprocmask(SIG_SETMASK,NULL,&before_mask));
    pid_t parent=getpid(),child=fork();assert(child>=0);
    if(!child){usleep(25000);int r=kill(parent,SIGTERM);_exit(r?1:0);}
    sio_state s;cancelled=0;assert(!sio_init(&s,"1",cancel_test));
    int n=writer?sio_write(&s,fd[0],b,192):sio_read(&s,fd[0],b,192);
    assert(n==-1&&errno==ECANCELED&&s.cancelled_io==1);
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(fcntl(fd[0],F_GETFL)==before);
    assert(!sigprocmask(SIG_SETMASK,NULL,&after_mask));
    assert(sigismember(&before_mask,SIGINT)==sigismember(&after_mask,SIGINT));
    assert(sigismember(&before_mask,SIGTERM)==sigismember(&after_mask,SIGTERM));
    assert(writer?s.write_waits==1:s.read_waits==1);
    close(fd[0]);close(fd[1]);
    printf("PASS real blocked %s cancellation with SA_RESTART, mask/fd flags preserved\n",writer?"write":"read");
}
int main(void)
{
    struct sigaction sa={0},oldterm,oldpipe;
    sa.sa_handler=term;sa.sa_flags=SA_RESTART;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGTERM,&sa,&oldterm));
    sa.sa_handler=pipe_seen;assert(!sigaction(SIGPIPE,&sa,&oldpipe));
    blocked(0);blocked(1);
    int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    close(fd[1]);cancelled=0;sio_state s;assert(!sio_init(&s,"1",cancel_test));
    char b[4]={0};assert(sio_write(&s,fd[0],b,2)==-1&&errno==EPIPE&&!pipe_signals);
    assert(sio_read(&s,fd[0],b,2)==0);close(fd[0]);
    /* Exercise the remaining pure helpers in this compilation too. */
    void *p=b;int count=2,adjust=-1,delay=2;
    assert(!sio_skip(&s,&p,&count,&adjust,&delay));assert(!sio_padding(&s,1,sizeof b,0));
    sio_report(&s);
    assert(!sigaction(SIGTERM,&oldterm,NULL));assert(!sigaction(SIGPIPE,&oldpipe,NULL));
    puts("SOCKET IO REAL WAIT CHECKS PASS: clean EOF, EPIPE without SIGPIPE, no busy polling");return 0;
}
