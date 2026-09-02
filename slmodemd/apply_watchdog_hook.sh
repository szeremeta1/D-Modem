#!/usr/bin/env bash
# Interpose VPcmV34Progress so the V.34 carrier-loss watchdog can be corrected.
#
# NO VENDOR INSTRUCTION IS MODIFIED and no vendor object is redistributed. This
# runs objcopy against YOUR copy of dsplibs.o from your own sl-modem-daemon
# tree. Nothing here ships Smart Link's code.
#
# Why: dsplibs.o emits its internal call to VPcmV34Progress as an R_386_PC32
# relocation against a global symbol rather than resolving it at assembly time,
# so ordinary link-time symbol resolution is enough to place a wrapper in the
# path.  --weaken-symbol makes the vendor body losable; --add-symbol names it so
# the wrapper can still call it.
#
# Usage: apply_watchdog_hook.sh <slmodem-src-dir>
set -euo pipefail
SRC=${1:?usage: apply_watchdog_hook.sh <slmodem-src-dir>}
SYM=VPcmV34Progress
cd "$SRC"
[ -f dsplibs.o ] || { echo "no dsplibs.o in $SRC" >&2; exit 1; }
if nm dsplibs.o | grep -q "${SYM}__orig"; then
    echo "already hooked"; exit 0
fi
# Look the address up rather than hard-coding it: it differs between builds.
A=$(nm dsplibs.o | awk -v n="$SYM" '$3==n && $2=="T" {print $1}')
[ -n "$A" ] || { echo "$SYM is not a defined text symbol in dsplibs.o" >&2; exit 1; }
echo "== $SYM at .text+0x$A"
objcopy --weaken-symbol=$SYM \
        --add-symbol "${SYM}__orig=.text:0x${A},global,function" \
        dsplibs.o dsplibs.hooked.o
mv dsplibs.hooked.o dsplibs.o
cp "$(dirname "$0")/slm_v34_watchdog.c" .
if ! grep -q 'slm_v34_watchdog.o' Makefile; then
    sed -i 's|^\(all-objs:= .*\)dsplibs.o|\1slm_v34_watchdog.o dsplibs.o|' Makefile
fi
grep -q 'slm_v34_watchdog.o' Makefile || { echo "Makefile edit failed" >&2; exit 1; }
make -s slmodemd
# ---- assert the interposition actually took. A weakened symbol that fails to
# bind produces a build that looks correct and changes nothing.
W=$(objdump -d slmodemd | grep -c "call.*<${SYM}>$"       || true)
O=$(objdump -d slmodemd | grep -c "call.*<${SYM}__orig>$" || true)
echo "== call sites: -> wrapper $W, -> original $O"
[ "$W" -ge 1 ] || { echo "ASSERT FAIL: no call site reaches the wrapper" >&2; exit 1; }
[ "$O" -ge 1 ] || { echo "ASSERT FAIL: the wrapper never calls the original" >&2; exit 1; }
echo "== OK"
