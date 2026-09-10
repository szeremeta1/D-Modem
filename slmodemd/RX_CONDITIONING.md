# Experimental signed-linear receive conditioning

This optional module provides a reproducible way to test the RX noise and
self-echo proposal in [D-Modem issue #13](https://github.com/strozfriedberg/D-Modem/issues/13#issuecomment-5619094216).
It operates on 16-bit linear PCM immediately before `modem_process`, at the DSP
sample rate. It observes the resulting TX samples through a const pointer and
never changes that generated TX buffer. It does not add noise to transmitted
PCM or to G.711 codewords. Altered RX can naturally change what the modem chooses
to transmit on later DSP iterations.

The proposed mechanism remains a hypothesis to test on each path. Component
checks establish the intended sample operations; they do not establish faster
negotiation, V.90 operation, better throughput, or an improved dropped-call rate.
This module neither changes the vendor carrier-loss threshold nor disables its
echo canceller.

## Build without changing the input tree

On Linux, install the normal D-Modem 32-bit compiler/development dependencies,
GNU make and binutils, Python 3.9 or newer, and GNU coreutils. From the repository
root:

```sh
python3 slmodemd/apply_rx_conditioning.py slmodemd
```

This builds `slmodemd-rxcond/slmodemd` using the source tree's own Makefile plus
libm. The optional second argument chooses another output directory. The source
directory and `dsplibs.o` are unchanged; no vendor instructions or private DSP
object offsets are involved. A temporary output is compiled and checked before
it becomes the requested output directory. Any existing destination is refused,
including an earlier successful build; choose a fresh directory to rebuild.

`rx-conditioning-build.json` records the source, copied header, output binary,
and unchanged vendor-object SHA-256 values. This is a build record, not call
acceptance. No modem process or SIP call is started by the installer or tests.

Run the resulting binary with the same command-line arguments as the original.
For example, an **unconditioned experimental control** is:

```sh
DMODEM_RXCOND=1 ./slmodemd-rxcond/slmodemd -d2 -e ./d-modem /dev/slamr0
```

A **combined noise/echo arm** uses that same binary and bounded processing blocks:

```sh
DMODEM_RXCOND=1 DMODEM_RX_NOISE_DBFS=-65 DMODEM_RX_ECHO_DB=-40 \
  DMODEM_RX_ECHO_SAMPLES=192 \
  ./slmodemd-rxcond/slmodemd -d2 -e ./d-modem /dev/slamr0
```

Without `DMODEM_RXCOND=1`, the original socket reads, writes, and sample buffers
are retained. Other conditioning variables are ignored. Setting `DMODEM_RXCOND=1`
alone deliberately enables the same experimental read/processing behavior as
the conditioned arms, with zero noise, zero echo, and unity RX gain. Comparing
the enabled control against enabled interventions avoids confounding noise or
echo with different socket read boundaries.

## Controls

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `DMODEM_RXCOND` | `0` | `1` enables the experimental host path; only 0 or 1 is accepted. |
| `DMODEM_RX_NOISE_DBFS` | Omitted: no noise | Uniform pseudorandom noise RMS relative to 32768. Range -120 to -20 dBFS. |
| `DMODEM_RX_ECHO_DB` | Omitted: no echo | Amplitude gain of delayed local TX added to RX. Range -120 to -10 dB; -40 means multiplication by 0.01. |
| `DMODEM_RX_ECHO_SAMPLES` | DSP rate / 50 | Delay in processed DSP samples. An integer from 1 through 4096. At 9600 Hz the default is 192 samples, or 20 ms. |
| `DMODEM_RX_GAIN_DB` | `0` | Gain applied to original RX before adding echo/noise. Range -24 to 0 dB. |

Invalid enabled settings reject socket startup. Omitted noise or echo means
exactly zero addition; explicitly setting -120 dB is a small nonzero addition.
The generated noise uses a fixed per-call seed for reproducibility. At -65 dBFS,
its requested RMS is approximately 18.4266 signed-PCM units; uniform peak
amplitude is approximately 31.9162. Summation saturates at the int16 limits before
rounding. The shutdown summary reports clipped samples so saturation cannot be
mistaken for a benign intervention.

Processing blocks are bounded by the smaller of 20 ms and the configured echo
delay. This ensures every required TX reference is already available and avoids
letting asynchronous read sizes select a different echo delay. Reads preserve an
odd trailing stream byte until its partner arrives. EOF halfway through a PCM
sample is an error. Short socket writes are rejected rather than accepted as a
complete sample stream. Call startup resets counters, history, and any pending
byte. Call shutdown reports sample, clipping, and host-delay-adjustment counts.

## Processed-sample echo delay is not host I/O delay

`DMODEM_RX_ECHO_SAMPLES=192` means RX sample number k receives a scaled copy of
processed TX sample k-192. It does **not** assert that the socket loop, converter,
packet path, or telephone line has a measured 20 ms delay.

The host driver's `MDMCTL_IODELAY` value is a separate input to the vendor DSP.
This module does not change it. In stock D-Modem, the socket driver still reports
zero. Thus the combined example above implements the noise/echo sample operations,
not every part of the issue comment's suggested configuration. Selecting a host
I/O delay requires a separate measured experiment.

The existing driver's `m->update_delay` path can discard input samples or write
additional output silence outside the processed-sample history. Such adjustments
are logged with the processed sample index and counted at shutdown. When any
occur, do not equate the synthetic delay with the entire host loop. The module
does not reinterpret or repair that existing driver behavior.

## Component and build checks

From the repository root:

```sh
cc -O2 -Wall -Wextra slmodemd/tests/rx-conditioning-test.c -lm -o /tmp/rxcond-test
/tmp/rxcond-test
# Linux with the normal 32-bit D-Modem build dependencies:
python3 slmodemd/tests/test_rx_conditioning_build.py
```

The C check covers inactive bypass, active unconditioned control, noise mean/RMS
and lag-one correlation, odd-byte stream preservation and partial-sample EOF,
sample-exact echo under variable processing sizes and history wrap, immutable TX,
saturation, call-state reset, and invalid settings. The build checks cover a real
i386 link against the public source/blob, source preservation, manifest hashes,
existing/symlink/nested destinations, compiler failure cleanup, a concurrent
destination, and incompatible source anchors. They never start a modem.

In the component run accompanying this extraction, 1,920,000 generated samples
measured -64.995822 dBFS RMS, mean -0.011148 PCM units, and lag-one correlation
-0.000579 for the -65 dBFS setting. This quantifies the generator, not telephone
line noise or a connection improvement.

For hardware comparisons, alternate control, noise-only, echo-only, and combined
arms on the same binary. Keep settings and per-device results separate. Record
all attempts, setup failures, both final negotiated rates, payload integrity and
throughput, retrains, held duration, and unexpected disconnects. An announced
CONNECT rate by itself is not end-to-end acceptance.
