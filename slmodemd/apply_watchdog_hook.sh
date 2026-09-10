#!/usr/bin/env bash
# Build a separate, opt-in V.34 watchdog binary; never alter the source tree.
# The wrapper uses private i386 offsets. Only the exact audited vendor object
# shipped in this D-Modem tree is supported. A symbol name alone is not an ABI.
# No vendor instructions are changed; objcopy changes only symbol metadata.
# Usage: apply_watchdog_hook.sh <slmodem-source-directory> [output-directory]
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd -P)
SRC=$(cd -- "${1:?usage: apply_watchdog_hook.sh <source> [output]}" && pwd -P)
OUT=${2:-${SRC}-watchdog}
PARENT=$(cd -- "$(dirname -- "$OUT")" && pwd -P)
OUT=$PARENT/$(basename -- "$OUT")
EXPECTED=1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379
SYM=VPcmV34Progress
NM=${NM:-nm}
OBJCOPY=${OBJCOPY:-objcopy}
OBJDUMP=${OBJDUMP:-objdump}
MAKE=${MAKE:-make}
STAGE=
cleanup() { [ -z "$STAGE" ] || rm -rf -- "$STAGE"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { echo "refusing: $*" >&2; exit 1; }
for tool in sha256sum "$NM" "$OBJCOPY" "$OBJDUMP" "$MAKE"; do
    command -v "$tool" >/dev/null || fail "required tool unavailable: $tool"
done
[ "$OUT" != "$SRC" ] || fail "output must differ from source"
case "$OUT/" in "$SRC/"*) fail "output must not be inside source";; esac
[ ! -L "$OUT" ] || fail "output must not be a symlink"
[ -f "$SRC/dsplibs.o" ] || fail "source has no dsplibs.o"
[ -f "$SRC/Makefile" ] || fail "source has no Makefile"
ACTUAL=$(sha256sum "$SRC/dsplibs.o" | awk '{print $1}')
[ "$ACTUAL" = "$EXPECTED" ] || fail "unsupported dsplibs.o SHA-256 $ACTUAL; no files changed"

# Freeze the source inputs and hook before building. Existing generated object
# files are discarded in the isolated copy; the audited vendor blob is retained.
STAGE=$(mktemp -d "$PARENT/.slm-v34-watchdog.XXXXXX")
cp -a "$SRC/." "$STAGE/"
cp "$HERE/slm_v34_watchdog.c" "$STAGE/slm_v34_watchdog.c"
(
    cd "$STAGE"
    find . -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name Makefile -o -name dsplibs.o \) -print0 |
        sort -z | xargs -0 sha256sum > .watchdog-inputs.sha256
)

verify_binary() {
    local dir=$1 wrapper original
    "$NM" "$dir/slmodemd" > "$STAGE/.watchdog-nm"
    wrapper=$(awk -v s="$SYM" '$3==s && $2=="T" {print $1}' "$STAGE/.watchdog-nm")
    original=$(awk -v s="${SYM}__orig" '$3==s && $2=="T" {print $1}' "$STAGE/.watchdog-nm")
    [ -n "$wrapper" ] && [ -n "$original" ] && [ "$wrapper" != "$original" ] ||
        fail "linked wrapper and original are not distinct strong symbols"
    "$OBJDUMP" -d "$dir/slmodemd" > "$STAGE/.watchdog-disassembly"
    # Verify the actual vendor caller, not any unrelated call in the executable.
    awk '/^[[:xdigit:]]+ <vpcm_run>:/ {p=1;next} p && /^[[:xdigit:]]+ <.*>:/ {exit} p' \
        "$STAGE/.watchdog-disassembly" > "$STAGE/.watchdog-caller"
    grep -Eq "[[:space:]]call[lq]?[[:space:]].*<${SYM}>$" "$STAGE/.watchdog-caller" ||
        fail "vpcm_run does not call the wrapper"
    awk -v s="$SYM" '$0 ~ "^[[:xdigit:]]+ <" s ">:" {p=1;next} p && /^[[:xdigit:]]+ <.*>:/ {exit} p' \
        "$STAGE/.watchdog-disassembly" > "$STAGE/.watchdog-wrapper"
    grep -Eq "[[:space:]](call|jmp)[lq]?[[:space:]].*<${SYM}__orig>$" "$STAGE/.watchdog-wrapper" ||
        fail "wrapper does not reach the original"
}

# Repeat invocations verify both inputs and the installed output before saying
# 'already built'. Changed or unrelated output is never overwritten: choose a
# new output directory. In particular, a half-finished install is not success.
if [ -e "$OUT" ]; then
    [ -d "$OUT" ] && [ -f "$OUT/.watchdog-output.sha256" ] || fail "output already exists and is not a completed watchdog build"
    cmp -s "$STAGE/.watchdog-inputs.sha256" "$OUT/.watchdog-inputs.sha256" || fail "build inputs changed; choose a new output directory"
    (cd "$OUT" && sha256sum -c --status .watchdog-output.sha256) || fail "existing output changed; choose a new output directory"
    verify_binary "$OUT"
    echo "verified existing build: $OUT/slmodemd"
    exit 0
fi
(
    cd "$STAGE"
    "$NM" dsplibs.o > .watchdog-nm
    address=$(awk -v s="$SYM" '$3==s && $2=="T" {print $1}' .watchdog-nm)
    [ "$address" = 0000b3c0 ] || fail "audited symbol offset differs"
    "$OBJCOPY" --weaken-symbol="$SYM" \
        --add-symbol "${SYM}__orig=.text:0x${address},global,function" \
        dsplibs.o dsplibs.hooked.o
    mv dsplibs.hooked.o dsplibs.o
    awk '/^all-objs:=/ {sub(/dsplibs[.]o/, "slm_v34_watchdog.o dsplibs.o")} {print}' Makefile > Makefile.watchdog
    mv Makefile.watchdog Makefile
    grep -q 'slm_v34_watchdog.o dsplibs.o' Makefile || fail "Makefile insertion failed"
    find . -maxdepth 1 -type f -name '*.o' ! -name dsplibs.o -delete
    rm -f slmodemd modem_test .build_profile .depend
    "$MAKE" -s slmodemd
)
verify_binary "$STAGE"
(
    cd "$STAGE"
    rm -f .watchdog-nm .watchdog-disassembly .watchdog-caller .watchdog-wrapper
    find . -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name Makefile -o -name dsplibs.o -o -name slmodemd -o -name .watchdog-inputs.sha256 \) -print0 |
        sort -z | xargs -0 sha256sum > .watchdog-output.sha256
)
# The complete tree becomes visible only after compilation and verification.
# Do not overwrite or nest inside an output created concurrently. GNU mv -n
# can report success without moving, so verify that the stage disappeared.
mv -T -n -- "$STAGE" "$OUT" || fail "output appeared during build or rename failed; source left untouched"
[ ! -e "$STAGE" ] || fail "output appeared during build; left untouched"
STAGE=
echo "built and verified: $OUT/slmodemd"
echo "opt in at startup with SLM_V34_LOWSIG=-1000000"
