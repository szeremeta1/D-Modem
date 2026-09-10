#!/usr/bin/env python3
"""Compile/run isolated ABI-wrapper checks; no modem, network, or vendor code."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "slm_v34_watchdog.c"
HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
extern int wrap(void *, float *, float *, int, int *, int *, int *, int *)
    __asm__("VPcmV34Progress");
int orig(void *, float *, float *, int, int *, int *, int *, int *)
    __asm__("VPcmV34Progress__orig");
static void *object;
static float input[48], output[48];
static int a, b, c, d, calls, expected, check_threshold;
int orig(void *obj, float *in, float *out, int n,
         int *rxbits, int *nrx, int *txbits, int *ntx)
{
    int observed;
    assert(obj == object && in == input && out == output && n == 48);
    assert(rxbits == &a && nrx == &b && txbits == &c && ntx == &d);
    if (check_threshold) {
        memcpy(&observed, (char *)obj + 0x230, sizeof observed);
        assert(observed == expected);
        /* Simulate the vendor resetting its threshold during a retrain.
           The next invocation uses this very same object address. */
        observed = 101;
        memcpy((char *)obj + 0x230, &observed, sizeof observed);
    }
    ++calls;
    *nrx = calls;
    *ntx = calls + 1;
    return 9;  /* A fatal result must also pass through unchanged. */
}
int main(int argc, char **argv)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int i, metric = argc > 1 && strcmp(argv[1], "metric") == 0;
    check_threshold = argc > 1 && !metric;
    if (check_threshold) expected = (int)strtol(argv[1], NULL, 10);
    object = mmap(NULL, page, (check_threshold || metric) ? PROT_READ | PROT_WRITE : PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(object != MAP_FAILED);
    for (i = 0; i < 4; ++i) {
        assert(wrap(object, input, output, 48, &a, &b, &c, &d) == 9);
        assert(calls == i + 1 && b == calls && d == calls + 1);
    }
    assert(munmap(object, page) == 0);
    puts("ok");
    return 0;
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix="dmodem-watchdog-test-") as tmp:
        root = Path(tmp)
        harness = root / "harness.c"
        harness.write_text(HARNESS)
        exe = root / "test-watchdog"
        command = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(command + ["-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O2",
                                  "-DSLM_WATCHDOG_TEST", str(SOURCE), str(harness),
                                  "-o", str(exe)], check=True)
        baseline = {k: v for k, v in os.environ.items() if not k.startswith("SLM_V34_")}
        cases = [
            ("default passthrough with inaccessible object", {}, [], False, 0),
            ("historical control", {"SLM_V34_LOWSIG": "1"}, [], False, 0),
            ("override reapplied after vendor reset on same object", {"SLM_V34_LOWSIG": "-1000000"}, ["-1000000"], False, 0),
            ("signed lower bound", {"SLM_V34_LOWSIG": str(-(2**31))}, [str(-(2**31))], False, 0),
            ("zero is an explicit threshold", {"SLM_V34_LOWSIG": "0"}, ["0"], False, 0),
            ("reject truncated scientific notation", {"SLM_V34_LOWSIG": "-1e6"}, [], True, 0),
            ("reject overflow", {"SLM_V34_LOWSIG": "9999999999999999999999"}, [], True, 0),
            ("reject trailing characters", {"SLM_V34_LOWSIG": "0bad"}, [], True, 0),
            ("metric only preserves threshold", {"SLM_V34_METRIC": "2"}, ["metric"], False, 2),
            ("zero metric uses exact passthrough", {"SLM_V34_METRIC": "0"}, [], False, 0),
        ]
        for name, settings, args, invalid, metric_lines in cases:
            result = subprocess.run([str(exe)] + args, env=baseline | settings,
                                    text=True, capture_output=True, check=True)
            assert result.stdout == "ok\n", (name, result.stdout)
            lines = result.stderr.splitlines()
            assert sum(line.startswith("V34METRIC ") for line in lines) == metric_lines, (name, lines)
            assert sum("invalid " in line for line in lines) == int(invalid), (name, lines)
            assert len(lines) == metric_lines + int(invalid), (name, lines)
            print("PASS", name)


if __name__ == "__main__":
    run()
