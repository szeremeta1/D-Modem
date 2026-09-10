# Experimental near-end V.34 echo output bypass

This opt-in source wrapper suppresses the echo **estimate returned to two
verified near-context callers**. It still runs the original filter and returns
the original estimate to far and unknown callers. Direct offline checks exercise
the actual vendor callers, initializer, filter and adaptation routines. No modem
call, waveform replay, rate improvement or reliability gain is claimed.

Only the public i386 `dsplibs.o` with this SHA-256 is accepted:

```text
1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379
```

## Build from a known prebuilt baseline

Build the stock host once with a working Linux GCC i386 toolchain, then retain
its compiled objects for the experiment:

```sh
make -C slmodemd slmodemd LFLAGS=-lm
python3 slmodemd/near-ec/build.py slmodemd /tmp/near-ec-output
python3 slmodemd/near-ec/check.py /tmp/near-ec-output
python3 slmodemd/near-ec/test_build.py slmodemd
```

The experiment builder never invokes make or recompiles host sources. It
compiles only `near_ec.c` and its offline fixtures, reuses every baseline host
object, and retains the baseline executable's PIE/EXEC layout type. This avoids
silently rebuilding unrelated source changes into the candidate. All reused
objects and the baseline executable are hashed before and after the build.

Work occurs in a staging directory outside the baseline and is published only
after validation. Existing, nested and symlink outputs are refused. Compilation
failure removes the staged output; a concurrently created output is preserved.
No binary is installed or started as a modem. `near-ec-build.json` records input,
wrapper and output hashes, unchanged DSP sections, and linked return markers.

The builder validates exact function sizes/addresses and the complete set of
three `R_386_PC32 V34EchoFilter` relocations before adding symbols. It refuses
competing wrappers in the prebuilt host objects. Every allocated instruction/data
section is compared byte-for-byte after symbol changes; no DSP instruction is
patched. The final modem and both offline executables must have a distinct
strong wrapper, all three exact E8 calls targeting it, and a call to the original
**inside the wrapper's own body**. An unrelated call elsewhere is insufficient.

## Startup control and intervention

`DMODEM_V34_NEAR_EC_BYPASS=1` enables the bypass. Missing, empty, `0` and `1junk`
values are disabled. The setting is read once at process startup; the per-sample
path performs no environment lookup or logging. It applies to those V.34 call
sites in the process, in either calling or answering role.

The wrapper uses linker return-address labels, not a remembered context pointer
or hard-coded runtime address. It always calls the original with the same
pointer and signed-short offset. It then returns zero only for the two near
labels when enabled. Far and unknown callers retain the original result.

This is an output intervention, not a CPU-cost optimization or a complete EC
disable. Working history still advances and adaptation remains enabled wherever
the vendor enables it. The changed receiver error can change later coefficients
and other downstream state. Whole-call internal state equivalence is not claimed.
The transmitted samples, TX prefilter and TX reference-history updates are not
modified by this wrapper.

## Verified object behavior

Addresses below are `.text` offsets in the exact accepted object.

| Evidence | Behavior |
|---|---|
| `v34FreezeEcho`, `0x5e200`, length `0xde` | Debug labels at `.rodata.str1.4+0xdc60` and `+0xdc88` identify `v34+0x80b8` as near and `v34+0x9138` as far independently of coefficient magnitudes. |
| `V34InitializeImplementationSpecific`, `0x71d70`, length `0xb5` | Both contexts have 144 taps and 1656 entries of circular TX history, with separate coefficient/fraction/working/circular arrays. |
| `V34EchoFilter`, `0x71ec0`, length `0x8b` | ABI: `int(void *context, short history_offset)`, i386 cdecl. The signed offset selects circular TX history; it is not a received audio sample. The function shifts working history and returns a signed coefficient dot product. |
| `modem_serrint`, call `0x5d419`, return `0x5d41e` | Passes near context `v34+0x80b8`. |
| `modem_serrint`, call `0x5d4f4`, return `0x5d4f9` | Passes far context `v34+0x9138` only when `uint16(v34+0xa23c)` is nonzero. |
| `adaptecho`, call `0x5d9ca`, return `0x5d9cf` | Passes near context `v34+0x80b8`. |
| Receiver correction | Adds `((near+far)*4+0x8000)>>16` to RX; coefficient sign determines the correction. Adaptation uses equivalently scaled near output. |

The three relocation offsets are exactly `0x5d41a`, `0x5d4f5` and `0x5d9cb`.
`V34EchoCleanUp` resets the write cursor and zeros coefficients, fractions and
working history. The separate TX path calls the 42-tap `V34EchoPreFilter`, then
updates near reference history from half the filtered TX sample and far history
from half the sample in the bulk-delay ring. That path remains intact.

A blanket flag is not a verified near-only control: `v34+0x25c2` bit `0x0200`
affects both the receiver filter branch and both TX histories; bit `0x0004`
freezes both adaptation paths without stopping their outputs. A zero tap count
is unsafe: the shift loop is entered with `taps-1` before the later zero check.
Neither workaround is used here.

## Actual offline checks and limits

`offline_check.c` uses explicit synthetic in-memory fixtures and real vendor
functions. No modem main loop is invoked. Both non-PIE and PIE test executables
run in separate processes for each of the five startup-option cases.

| Actual vendor path, synthetic RX=1000 | Disabled | Near bypass enabled |
|---|---:|---:|
| `modem_serrint`, far disabled | 1265 | 1000 |
| `modem_serrint`, far enabled | 1397 | 1132 |
| `adaptecho`, adaptation frozen | 1265 | 1000 |
| `adaptecho`, adaptation active | 1265 | 1000 |

The retained **132-unit far contribution** demonstrates that the enabled
candidate does not zero both estimates in the actual receiver caller. The
fixtures check working histories, ring contents/cursors and the relevant
coefficient/fraction state against the original functions. The active-adaptation
case independently checks the update expected from the changed residual.
Unknown callers preserve outputs for both contexts, signed offsets and ring wrap.

Build tests also reject a weak wrapper, every misdirected marker call, and an
original-call lookalike outside the wrapper. They exercise missing objects,
unknown DSP bytes, a competing prebuilt wrapper, failed compilation, and existing
or concurrent output preservation while a make stub refuses any make invocation.

These tests establish the intended local intervention and checked ABI. They do
not establish whether a physical modem path benefits from it. That requires
interleaved enabled/disabled hardware calls using one candidate binary and one
fixed transport baseline, measuring setup failures, rates, payloads, retrains
and drops. A few sequential calls or a count of distinct replay states cannot
substitute for those outcomes. Keep this intervention separate from digital
V.90 transport or RX noise/echo when interpreting an experiment.
