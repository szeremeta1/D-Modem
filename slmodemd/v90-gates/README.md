# Experimental V.90 digital-answer negotiation gates

This experiment selects the vendor's digital V.90 constructor, declares the
digital role in V.8, and preserves the mode-1 V.34 initialization that supplies
digital INFO0. It also provides an offline constructor/menu test. It does **not**
implement native 8 kHz PCM transport, perform a waveform handshake, or establish
a negotiated rate or reliability improvement. It is not a V.92 implementation.

The control defaults off. An enabled digital-answer experiment is accepted only
for an explicit V.90 answer request. Unsupported answers and V.34 fallback keep
the analog constructor. The ordinary caller path is retained.

## Build without touching the source or an installed modem

On Linux with Python 3, GNU binutils, make, and a working GCC i386 toolchain:

```sh
python3 slmodemd/v90-gates/build.py slmodemd /tmp/dmodem-v90-gates
cd /tmp/dmodem-v90-gates
timeout 15 ./offline-check
```

The output must be a new directory outside the input tree. Existing outputs,
including symlinks, are refused. Work is staged beside the output and published
only after both executables link and wrapper/host call targets are checked.
Failure leaves the input and an existing output intact. The script never
installs a binary or starts a modem process.

The builder accepts only the public `dsplibs.o` with SHA-256:

```text
1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379
```

It checks exact symbol addresses/sizes, eight symbolic call relocation sites,
the six original V.8 branch bytes, and the public source/Makefile anchors.
Unknown or already modified objects fail closed. It uses the stock public host
sources and Makefile; no separately supplied host objects are needed. Output
includes `gate-build.json` hashes and `gate-link-disassembly.txt`.

The original DSP object has executable-stack and text-relocation linker
warnings on current toolchains. The new offline test compiles with
`-Wall -Wextra -Werror`. These checks are not a security audit of the vendor blob.

Run the build/refusal regressions in disposable directories with:

```sh
python3 slmodemd/v90-gates/test_build.py
```

## Controls and lifecycle

`DMODEM_V90_DIGITAL_ANSWER=1` enables this experiment; other values leave it off.
Any eventual isolated line experiment must request V.90 explicitly
(`AT+MS=90,0`). Successful offline gates do not make the incomplete digital
transport suitable for production calls.

The builder inserts one host-source callback immediately before
`do_modem_change_dp` calls its selected datapump constructor. That callback
receives the actual requested datapump, caller/answer role, and engine ID.
The V.8 configuration alone cannot distinguish a V.90 answer request from V.92:
the vendor's `v8_create` clears the V.92 capability bit for both answer roles.
Inferring the request from that bit would incorrectly enable V.92.

A new V.8 transition arms only a possible **offer**, for an answering V.90
request. `V8Create` clears prior selection. The PCM constructor is armed only
after `V8UpdateModemParameters` succeeds, the remote decoded CM offers data,
V.34 and analog PCM capability, and the emitted JM contains valid digital
access/PCM declarations and a consistent whole-word bit count. An update that
rejects those requirements clears the selection. A host transition to V.34,
V.92, or a caller role clears it too. As in the host program, the gate state is
for one modem per process.

This distinction fixes an actual offline regression in the first prototype:
local offer capability armed the digital constructor even after the peer had
rejected PCM. A subsequent V.34 fallback then had a digital modulator, a null
demodulator, and PCM disabled. The regression now executes that fallback and
requires the ordinary analog object.

Cleanup follows the constructed object, independent of current control or
negotiation state. The vendor's `VPCMXF_SessionTermination` unconditionally
invokes an analog demodulator callback. A digital object has no demodulator;
the wrapper skips only that analog training-history callback. Normal object
and K56 deletion still run, including after the control is disabled.

## What the offline check establishes

The test inserts a synthetic **decoded** caller menu, then invokes the actual
vendor sequence builders and selection routine. It preserves the remote CM
and changes only the local menu. It constructs and deletes actual vendor V.PCM
objects using public host initialization. Driver start/stop stubs abort if
invoked; no line, SIP connection, replay, or modem training is performed.

| Case | Access word | PCM word | V.90 selected | Constructed side |
|---|---:|---:|---:|---|
| Disabled V.90 answer, capable analog peer | `0x161` | absent | 0 | analog |
| Enabled V.90 answer, peer lacks analog PCM | `0x161` | absent | 0 | analog |
| Enabled V.90 answer, capable analog peer | `0x163` | `0x1c5` | 1 | digital |
| Enabled V.92 answer request | `0x161` | absent | 0 | analog |

The positive case has a non-null modulator and null demodulator, preserves the
original V.34 pointer, uses mode 1 with 48-sample V.34 fragments, retains
PCM-enabled=1, and produces role `0x66`. Calling `V34SetINFO0dBits` sets its
marker at byte offset 24 to `0x001e`. The disabled V.90 control produces role
`0x65`, a null modulator, and a non-null demodulator.

Other executed checks cover actual V.34 fallback, a fresh V.8 object before
selection, rejection after earlier selection, malformed JM bit count, host
fallback after earlier selection, cleanup after disable/reset, and identical
V.92 caller initial capabilities/menu with the control enabled and disabled.
That last comparison concerns initialization, not a completed V.92 call.

The build tests verify relative paths containing spaces, source preservation,
unknown-object and incompatible-host refusal, existing-output preservation,
compilation-failure cleanup, and a concurrent output appearing during build.

## ABI details verified against the public object

`VPCMXF_Create` has this C argument order:

```c
void *VPCMXF_Create(int digital, void *v34, void *params,
                   unsigned fragment_ms, unsigned computational_mode);
```

After its 60-byte stack prologue, the first argument is at stack+`0x40` and
the second at stack+`0x44`. Instructions `0xfcf9`, `0xfd05/0xfd07`, `0xfd0c`,
and `0xfdb2/0xfdc2` turn the first argument's logical inverse into the C++
`V90ModemSide` argument. Instructions `0xfdb6/0xfdc6` preserve the second
argument as the V.34 object pointer. The demangled constructor order is:

```text
VPcmFloModem(void*, V90ModemSide, _tagModemParameters*, unsigned,
             V90ComputationalMode, V92ComputationalMode)
```

First argument 1 creates digital side enum 0. Replacing the second argument
with a side enum would corrupt the V.34 pointer. The offline pointer checks
confirm the digital modulator and analog demodulator distinction.

`vpcm_create` selects V.90 mode 1 at `0x3c3a..0x3c42` and calls
`VPcmV34Create(v34, !caller, fragment, params, mode)` at `0x3c70`.
Its mode-1 branch at `0xb0a0..0xb0d7` enables PCM, inverts the second argument,
performs the ordinary external reset, and selects the role. Passing answer
argument 0 through this mode-1 path yields digital role `0x66`. Forcing mode 5
would bypass the V.90 enable/reset setup and is not the intervention used here.

The V.8 builders emit analog access/PCM immediates at `0x759e6/0x759f4` and
`0x75ed4/0x75f08`. Strong source wrappers run after the original builders and
change the local declaration when the verified gate permits it. No remote
digital capability is fabricated to trick the original analog predicate.
The only DSP instruction-byte change is the checked six-byte branch at
`.text+0x3607`; wrappers and their original aliases remain distinct symbols.

## Transport remains unfinished

The digital transmitter's native 8 kHz PCM output is separate from the host's
9,600 Hz V.8/V.34 audio stream. `VPcmFloModem::runPcmModem` has an analog-only
branch controlled by object+`0x6120`; the digital constructor clears it. Forcing
that branch would enter code assuming a non-null analog demodulator.

A complete implementation must select and transport native digital output,
define a sample-counted handover from handshake audio, preserve G.711 codeword
identity and clocking through the actual network path, and verify incoming
V.34 audio and end-to-end payloads. Neither a menu marker nor a constructor
pointer proves any of those. No speed ceiling, higher rate, successful call,
or reliability target is claimed by this extraction.
