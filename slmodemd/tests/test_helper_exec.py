#!/usr/bin/env python3
"""Exercise the actual socket_start fork/exec boundary with disposable sockets.

No DSP, modem, listener, daemon, privileged identity or network is used. Extract
the complete source function so the failure regression tests the real control
flow, rather than a copy of the proposed fix. Run unchanged against the old
source to reproduce child fallthrough (status 99 instead of 127).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

PRELUDE = r'''
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#define DBG(...) ((void)0)
struct device_struct { int fd, delay; };
struct modem { void *dev_data; char *dial_string; };
static const char *modem_exec;
static char outbuf[1024];
'''
DRIVER = r'''
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--helper")) {
        int fd = atoi(argv[2]); size_t total = 0; char buf[384];
        while (total < sizeof buf) {
            ssize_t n = read(fd, buf + total, sizeof buf - total);
            if (n <= 0) return 71;
            total += (size_t)n;
        }
        for (size_t i=0; i<sizeof buf; i++) if (buf[i]) return 72;
        return 42;
    }
    if (argc != 3) return 73;
    signal(SIGPIPE, SIG_IGN);
    pid_t owner = getpid();
    struct device_struct dev = { .fd = -1, .delay = 0 };
    struct modem modem = { .dev_data = &dev, .dial_string = "--helper" };
    modem_exec = argv[1];
    int started = socket_start(&modem);
    /* The regression: returning here in the child re-enters daemon code. */
    if (getpid() != owner) _exit(99);
    int status; pid_t child;
    do { child = waitpid(-1, &status, 0); } while (child < 0 && errno == EINTR);
    if (child <= 0 || !WIFEXITED(status)) return 74;
    int code = WEXITSTATUS(status);
    if (dev.fd >= 0) {
        char byte;
        ssize_t n = read(dev.fd, &byte, 1);
        /* Linux can reset a socket whose peer exits with unread initial
         * PCM. Both EOF and this failure-only reset prove peer closure. */
        if (n != 0 && !(n < 0 && errno == ECONNRESET && code == 127)) return 75;
        close(dev.fd);
    }
    printf("child_status=%d parent_start=%d\n", code, started);
    return code == atoi(argv[2]) ? 0 : 76;
}
'''


def check(source):
    original = source.read_bytes()
    text = original.decode()
    start = text.index('static int socket_start (struct modem *m)')
    end = text.index('static int socket_stop (struct modem *m)', start)
    body = text[start:end]
    if body.count('fork()') != 1 or body.count('execl(') != 1:
        raise ValueError('socket_start boundary differs')
    records = []
    with tempfile.TemporaryDirectory(prefix='dmodem-exec-test-') as directory:
        root = Path(directory); c = root/'test.c'; binary = root/'test'
        c.write_text(PRELUDE + body + DRIVER)
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra',
                        '-Werror', str(c), '-o', str(binary)], check=True, timeout=30)
        denied = root/'not-executable'; denied.write_text('#!/bin/sh\nexit 0\n'); denied.chmod(0o600)
        missing_interpreter = root/'bad-interpreter'
        missing_interpreter.write_text('#!' + str(root/'missing-loader') + '\n'); missing_interpreter.chmod(0o700)
        cases = [('missing-helper', root/'missing', 127),
                 ('permission-denied', denied, 127),
                 ('missing-interpreter', missing_interpreter, 127),
                 ('successful-exec-and-pcm-prefix', binary, 42)]
        for name, executable, expected in cases:
            result = subprocess.run([str(binary), str(executable), str(expected)],
                                    capture_output=True, text=True, timeout=5)
            record = {'case': name, 'returncode': result.returncode,
                      'stdout': result.stdout, 'stderr': result.stderr}
            records.append(record)
            if result.returncode:
                raise AssertionError(json.dumps(record))
            if expected == 127 and 'slmodemd: exec helper:' not in result.stderr:
                raise AssertionError('exec error was not reported')
    if source.read_bytes() != original:
        raise ValueError('source changed during test')
    return {'source_sha256': hashlib.sha256(original).hexdigest(),
            'cases': records, 'physical_validation': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1]/'modem_main.c')
    args = parser.parse_args()
    print(json.dumps(check(args.source), indent=2))
