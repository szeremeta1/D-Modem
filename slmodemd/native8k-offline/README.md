# Offline native-8k and decoded-Ja probes

These three source probes exercise a digital V.90 transmitter constructor, a bounded parser for already descrambled/frame-aligned Ja bits, and a local decoded-event adapter into transmitter phase3. They do not implement waveform reception, a SIP or modem service, native transport, peer negotiation, payload transfer or a physical 56k/rate/reliability result.

These probes extend the existing [digital-gates experiment](../v90-gates/README.md), reviewed at commit `4439727757a7d8be2a6176a7147fd14c5146cbcc`. They rebuild against its public host sources and original DSP object. No private host objects or service binary are dependencies. A historical result from another build does not validate this extraction; use the separately recorded public-build receipt.

## Reproduce from public sources

Use Linux with an i386-capable GCC/libc toolchain, GNU binutils, make, strace and Python 3. `PUBLIC-ABI.json` pins the public DSP, host sources, gate sources, four probe sources and five generator/packer symbol addresses and sizes. Assertions remain enabled.

From the repository root, choose a nonexistent output directory outside this checkout:

```sh
python3 slmodemd/native8k-offline/test_build.py
python3 slmodemd/native8k-offline/build.py slmodemd /tmp/new-native8k-offline
python3 slmodemd/native8k-offline/validate.py /tmp/new-native8k-offline
```

The builder copies only the pinned public inventory into the private output, invokes the reviewed gate builder there, and links exactly the sixteen public objects listed in the ABI manifest. Existing objects, hidden Make includes and unlisted source files are excluded. The gate builder also links a daemon executable for its existing disassembly checks; neither builder starts that daemon. The three probe entrypoints replace `main`. Input and resulting object hashes are checked again after linking. A changed input, existing/overlapping output, failed build or timeout cannot produce a successful build receipt. Build commands have separate process groups and bounded deadlines, with inherited compiler/Make/modem controls removed.

The validator runs only the three hashed probes, each with a 15-second limit, closed inherited descriptors, disabled core dumps and explicit `network,open,openat,openat2,creat` syscall observation. Missing, malformed or ambiguous traces fail; observed network calls or normalized device-path opens fail. This selected strace observation is not a sandbox or proof against every device-access mechanism or pathname alias. Raw outputs and traces remain in the private output directory. `build.json` and `validation.json` record toolchain, source/object/binary hashes and measured assertions. They explicitly distinguish local generation from waveform reception and physical calls.

The thirteen offline package fixtures cover changed public headers, gates and DSP, symlinks, existing/overlapping destinations, omitted untrusted objects and Make includes, inherited build settings, failed/timed-out commands and incomplete syscall/result evidence. They do not open a modem or network connection. Real Linux build and execution evidence must accompany any published measured result.

## Public Linux validation, 2026-09-13

The [fresh build receipt](build-20260913.json) and [execution receipt](validation-20260913.json) record a successful build from the pinned public sources, followed by all three actual probe runs. Thirteen package fixtures passed. Each probe exited zero; its complete selected syscall trace contained two library/cache opens and no network call or device-path open.

| Probe | Measured public-build result |
| --- | --- |
| Native TX | Three identical 20,000-sample vectors, 19,856 nonzero samples each, zero mu-law round-trip mismatches; one/forty-sample chunks and common dispatch agree |
| Decoded Ja | 16,384 descriptor combinations; all 5,024 representative single-bit changes and all 5,024 truncated prefixes not accepted |
| Ja control | Valid generated 308-bit descriptor accepted; invalid and duplicate input blocked; forty native samples, 27 nonzero |

These are executed component assertions, not negotiation with a modem. The transmitter waits for missing peer events. No modem daemon, physical call, waveform receiver or native transport was exercised by this validation.

## Probe boundaries

- `tx_offline.c` constructs the digital side using the verified V.8/host gate and preserves the V.34 pointer. It explicitly resets the transmitter before phase3, checks silence, capacity and guarded output buffers, compares one/forty-sample chunks and the common dispatcher, and waits without inventing a peer receive event. Its final summaries are observations to verify on the public build, not a negotiated link.
- `ja_offline.c` compares the parser with the vendor's descriptor packer: 16,384 descriptor-boundary combinations plus bit corruption and truncated prefixes of five representative frames. The maximum framed length is 2,654 bits. This exercises generated, already decoded and aligned bits, not a receive waveform or all ITU semantic constraints.
- `ja_control_offline.c` uses a constructor-owned adapter with pointer/state checks. A complete valid descriptor can enter phase3; malformed input preserves state and duplicate input is rejected. It generates a forty-sample local output block. The printed word “symbols” is historical fixture terminology for these local PCM samples, not evidence of peer acceptance.

The descriptor parser's caller must already have verified role, law, INFO1a and protocol state. Waveform timing/equalization, differential decoding/descrambling, Ja/S/CP/E reception, retrain/lifetime handling and an exact 8k TX versus 9.6k RX transport remain unfinished. See [ITU-T V.90](https://www.itu.int/rec/T-REC-V.90-199809-I/en). Local menu bits and generated vectors do not establish a physical clock ceiling or higher rates.

The existing project and slmodemd `COPYING` files are retained by the surrounding repository. This package contains newly written probe/parser source only; no vendor object, generated binary, private configuration or additional redistribution grant is included.
