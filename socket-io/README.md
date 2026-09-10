# Optional socket stream I/O experiment

This experiment preserves signed 16-bit sample bytes across fragmented socket
reads and partial writes. It is **disabled by default**. It has passed offline
component, fault, signal-cancellation and host-integration tests on Linux i386;
these tests do not establish better negotiated rates, successful physical calls,
or a call-drop percentage. No hardware or carrier was used in this validation.

With `DMODEM_SOCKET_IO=1`, the socket driver retains an odd input byte until its
sample is complete, retries only the unsent suffix of an output block, and waits
for socket readiness after `EAGAIN`. A signal-aware `ppoll` wait can be cancelled
even when the signal handler uses `SA_RESTART`. Per-operation nonblocking flags
leave the descriptor's file flags unchanged. Clean EOF returns zero; EOF halfway
through a sample is an error. Output uses `MSG_NOSIGNAL` so a closed peer returns
an error instead of terminating the process with SIGPIPE.

The enabled path also applies negative input-delay adjustments in two-byte sample
units, carries an adjustment across short blocks, and checks positive output-delay
padding before writing the block or clearing the padding buffer. It does not add
noise, echo, resampling, a read-size cap, packet framing, or a periodic clock. The
vendor DSP objects and ALSA/hardware driver paths remain unchanged.

Unset or exactly `DMODEM_SOCKET_IO=0` selects the original socket behavior.
Exactly `1` enables this experiment; an empty value or any other string is
rejected at socket startup. Restart the process to change the setting. Compare
both settings using the same candidate binary and otherwise identical controls.

## Build boundary

The builder targets public upstream `master` commit
`636959b37b592b87a47c6da2069149961cd70ccf`. It accepts only the `public-stock`
profile: the exact source, prebuilt host object, DSP object and baseline executable
hashes in [build.py](build.py). The prebuilt object and executable pins identify a
particular reviewed build, including its compiler and debug information. A fresh
build of the same source may therefore be refused. This repository does not ship
those generated objects or binaries. Supporting another baseline requires an
independent review and a new explicit profile; do not simply replace the pins
after a refusal.

On Linux with Python 3, GCC's 32-bit development support and GNU binutils, point
the builder at a reviewed `slmodemd/` build directory containing
the source, headers, objects and executable. The output must be a new directory:

```sh
python3 socket-io/build.py /path/to/reviewed/modem /tmp/dmodem-socket-io-build
```

The builder never invokes Make or modifies the input directory. It relinks the
original prebuilt objects and requires the result to reproduce the baseline
executable exactly. It also compiles the pristine host source and compares every
allocated ELF section and symbolic relocation against the original host object,
excluding path-dependent debug information. Only then does it compile the patched
host object and relink the candidate, reusing and rechecking every other object.
The output manifest records hashes and these checks. A failed build can leave its
new output directory for inspection; choose a new output directory for a retry.

## Offline checks

Run these from the repository root, using the same reviewed input and generated
output as above. The C tests use local socketpairs or fake I/O, never a modem:

```sh
python3 socket-io/test_build.py /path/to/reviewed/modem
python3 socket-io/test_integration.py /tmp/dmodem-socket-io-build
gcc -m32 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror socket-io/test_socket_io.c -o /tmp/dmodem-socket-io-test
/tmp/dmodem-socket-io-test
gcc -m32 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror socket-io/test_socket_wait.c -o /tmp/dmodem-socket-wait-test
/tmp/dmodem-socket-wait-test
```

Tests cover fragmented reads, odd-byte completion, short writes, EINTR/EAGAIN,
zero-progress and fatal I/O, half-sample EOF, signal cancellation with SA_RESTART,
unchanged signal masks/file flags, call reset, skipped samples and padding bounds.
The host integration fixture compiles the actual generated source with fake
OS/DSP boundaries and tests disabled behavior, device isolation, startup output,
delay adjustments and stop/start. Builder tests refuse changed inputs, symlinks,
an existing output, a duplicate patch and a changed header/compiler code result.

## Evidence and failure interpretation

Enabled calls normally report at `socket_stop`:

```text
socket_io: enabled=1 rx_bytes=N tx_bytes=N odd_reads=N short_writes=N read_eintr=N write_eintr=N read_waits=N write_waits=N io_errors=N cancelled=N instrument_errors=N truncated_samples=N skipped=N skip_requests=N
```

There is no successful-startup banner, and disabled calls emit no `socket_io`
statistics. Verify the executing binary and process environment to attribute an
arm. `INSTRUMENT ERROR` or a nonzero `instrument_errors` counter invalidates the
experiment. Ordinary I/O failures and truncated samples need their actual call
context; cancellation may be intentional cleanup. Fatal output errors may bypass
the normal final report. Missing logs alone prove neither a modem drop nor a
successful call. Preserve every attempted call and use independent end-to-end
payload and teardown evidence before making reliability claims.

[validation.json](validation.json) records the offline extraction check, with
public source and input hashes. Candidate executable hashes can vary with build
paths because debug information is retained.
