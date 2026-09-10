#!/usr/bin/env python3
"""Compile the actual patched host source with fake OS/DSP boundaries.

No modem constructor, child, device, external network or daemon is started.
Usage: test_integration.py SOCKET_IO_BUILD_DIRECTORY
"""
import json
from pathlib import Path
import subprocess
import sys

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
static ssize_t fake_read(int,void *,size_t);
static ssize_t fake_write(int,const void *,size_t);
static ssize_t fake_recv(int,void *,size_t,int);
static ssize_t fake_send(int,const void *,size_t,int);
static int fake_pair(int,int,int,int[2]);
static pid_t fake_fork(void);
static pid_t fake_wait(int *);
static int fake_close(int);
static char *fake_getenv(const char *);
#define read fake_read
#define write fake_write
#define recv fake_recv
#define send fake_send
#define socketpair fake_pair
#define fork fake_fork
#define wait fake_wait
#define close fake_close
#define getenv fake_getenv
#define main unused_driver_main
#define modem_process capture_modem_process
#include "modem_main.c"
#undef main
const char *modem_exec="offline-never-executed";
static char *flag;
static unsigned legacy_reads,legacy_writes,receives,sends,pairs;
static unsigned process_calls,process_count;
static void *process_input;
static unsigned char emitted[8192];
static size_t emitted_len,send_chunk,recv_chunk;
static int legacy_result;
static void reset_test(char *v)
{
    flag=v;legacy_reads=legacy_writes=receives=sends=pairs=0;
    process_calls=process_count=0;emitted_len=0;
    send_chunk=recv_chunk=8192;legacy_result=1;keep_running=1;
}
static char *fake_getenv(const char *key){assert(!strcmp(key,"DMODEM_SOCKET_IO"));return flag;}
static int fake_pair(int domain,int type,int protocol,int fd[2])
{assert(domain==AF_UNIX&&type==SOCK_STREAM&&!protocol);++pairs;fd[0]=40;fd[1]=41;return 0;}
static pid_t fake_fork(void){return 123;}
static pid_t fake_wait(int *status){(void)status;return 123;}
static int fake_close(int fd){assert(fd==40||fd==41);return 0;}
static ssize_t fake_read(int fd,void *buf,size_t n)
{(void)fd;assert(n>=1);++legacy_reads;memset(buf,0x5a,n);return legacy_result;}
static ssize_t fake_write(int fd,const void *buf,size_t n)
{(void)fd;(void)buf;(void)n;++legacy_writes;return legacy_result;}
static ssize_t fake_recv(int fd,void *buf,size_t n,int flags)
{assert(fd==41&&flags==MSG_DONTWAIT);++receives;if(n>recv_chunk)n=recv_chunk;memset(buf,0x5a,n);return n;}
static ssize_t fake_send(int fd,const void *buf,size_t n,int flags)
{assert(fd==41&&flags==(MSG_DONTWAIT|MSG_NOSIGNAL));++sends;if(n>send_chunk)n=send_chunk;
assert(n+emitted_len<=sizeof emitted);memcpy(emitted+emitted_len,buf,n);emitted_len+=n;return n;}
void capture_modem_process(struct modem *m,void *in,void *out,int count)
{(void)m;(void)out;++process_calls;process_count=count;process_input=in;}
'''

TEST = r'''
int main(void)
{
    struct device_struct dev={.fd=41},foreign={.fd=99};
    struct modem m;memset(&m,0,sizeof m);m.dev_data=&dev;
    char buf[384];
    /* Disabled startup and read/write wrappers execute the original syscalls,
     * even reproducing a one-byte result becoming zero samples. */
    reset_test(NULL);assert(socket_start(&m)==0&&pairs==1&&legacy_writes==1&&!sends&&dev.delay==0);
    assert(mdm_device_read(&dev,buf,192)==0&&legacy_reads==1&&!receives);
    assert(mdm_device_write(&dev,buf,192)==0&&legacy_writes==2&&!sends);
    m.update_delay=-48;dev.delay=192;
    assert(run_input(&m,&dev,192)==0&&process_calls==1&&process_count==144&&process_input==inbuf);
    assert(socket_stop(&m)==0&&!socket_io.enabled);
    reset_test("bad");assert(socket_start(&m)==-EINVAL&&!pairs&&!legacy_writes&&!sends);
    reset_test("0");assert(socket_start(&m)==0&&legacy_writes==1&&!sends);
    assert(socket_stop(&m)==0);
    reset_test("1");send_chunk=1;
    assert(socket_start(&m)==0&&pairs==1&&!legacy_writes&&sends==384&&dev.delay==192);
    assert(emitted_len==384);for(size_t i=0;i<384;i++)assert(!emitted[i]);
    recv_chunk=1;assert(mdm_device_read(&dev,buf,192)==1&&receives==2&&!legacy_reads);
    assert(mdm_device_read(&foreign,buf,192)==0&&legacy_reads==1&&receives==2);
    assert(mdm_device_write(&foreign,buf,192)==0&&legacy_writes==1);
    m.update_delay=-48;dev.delay=192;process_calls=0;
    assert(run_input(&m,&dev,192)==0&&process_calls==1&&process_count==144&&process_input==inbuf+96);
    m.update_delay=-250;dev.delay=500;process_calls=0;
    assert(run_input(&m,&dev,192)==0&&!process_calls&&m.update_delay==-58&&dev.delay==308);
    assert(run_input(&m,&dev,192)==0&&process_calls==1&&process_count==134&&process_input==inbuf+116);
    m.update_delay=-48;foreign.delay=192;process_calls=0;
    assert(run_input(&m,&foreign,192)==0&&process_calls==1&&process_count==144&&process_input==inbuf);
    device_write=mdm_device_write;
    emitted_len=0;sends=0;send_chunk=8192;m.update_delay=2049;dev.delay=192;
    assert(run_output(&m,&dev,192)==-1&&!sends&&!emitted_len&&socket_io.instrument_errors==1);
    socket_io.instrument_errors=0;
    assert(run_padding(&m,&dev)==-1&&!sends&&!emitted_len&&socket_io.instrument_errors==1);
    m.update_delay=2048;dev.delay=192;
    assert(run_padding(&m,&dev)==0&&emitted_len==4096&&sends==1&&dev.delay==2240&&!m.update_delay);
    socket_io.pending=1;
    assert(socket_stop(&m)==0&&!socket_io.enabled&&!socket_io.pending&&!socket_io_device);
    reset_test("1");assert(socket_start(&m)==0&&!socket_io.pending&&!socket_io.rx_bytes);
    assert(socket_stop(&m)==0);
    puts("HOST INTEGRATION PASS: actual source startup/read/write routing, flag-off legacy behavior, foreign-device isolation, sample-correct skip, padding-before-write, per-call reset");
    return 0;
}
'''


def main():
    base = Path(sys.argv[1]).resolve()
    manifest = json.loads((base / "socket-io-build.json").read_text())
    source = (base / "modem_main.c").read_text()
    a = source.index("\t\t\tin = inbuf;")
    b = source.index("\n", source.index("\t\t\tmodem_process(", a))
    input_block = source[a:b]
    output_start = b
    a = source.index("\t\t\tif(m->update_delay > 0) {")
    b = source.index("\n\t\t}\n\t\tif(FD_ISSET(m->pty", a)
    padding_block = source[a:b]
    output_block = source[output_start:b]
    scaffold = PRELUDE + "\nstatic int run_input(struct modem *m,struct device_struct *dev,int count) { void *in; for(int pass=0;pass<1;pass++) {\n" + input_block + "\n}return 0;}\n"
    scaffold += "static int run_padding(struct modem *m,struct device_struct *dev) {int count;\n" + padding_block + "\nreturn 0;}\n" + TEST
    position = scaffold.index("static int run_padding(")
    scaffold = scaffold[:position] + "static int run_output(struct modem *m,struct device_struct *dev,int count) {for(int pass=0;pass<1;pass++) {\n" + output_block + "\n}return 0;}\n" + scaffold[position:]
    target = base / "socket-io-host-integration.c"
    target.write_text(scaffold)
    binary = base / "socket-io-host-integration"
    subprocess.run(["gcc", "-m32", "-O2", "-ffunction-sections", "-fdata-sections", "-I"+str(base),
                    str(target), "-Wl,--gc-sections", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=10, check=True)
    print("profile=" + manifest["profile"])
    print(result.stdout, end="")
    (base / "host-integration.log").write_text(result.stdout)


if __name__ == "__main__": main()
