# D-Modem
Connect to dialup modems over VoIP using SIP, no modem hardware required.

https://www.aon.com/cyber-solutions/aon_cyber_labs/introducing-d-modem-a-software-sip-modem/

## Building
You'll need Linux and a working 32-bit development environment (gcc -m32 needs to work, Debian-based systems can install libc6-dev-i386), along with PJSIP's dependencies (OpenSSL).  Then run 'make' from the top-level directory.

## How it Works
Traditional “controller-based” modems generally used a microcontroller and a DSP to handle all aspects of modem communication on the device itself.  Later, so-called “Winmodems” were introduced that allowed for field-programmable DSPs and moved the controller and other functionality into software running on the host PC.  This was followed by “pure software” modems that moved DSP functionality to the host as well.  The physical hardware of these softmodems was only used to connect to the phone network, and all processing was done in software. 

D-Modem replaces a softmodem’s physical hardware with a SIP stack.  Instead of passing audio to and from the software DSP over an analog phone line, audio travels via the RTP (or SRTP) media streams of a SIP VoIP call.   

## Usage
The repository contains two applications: 

slmodemd – A stripped down and patched version of Debian’s sl-modem-daemon package.  All kernel driver code has been replaced with socket-based communication, allowing external applications to manage audio streams. 

d-modem – External application that interfaces with slmodemd to manage SIP calls and their associated audio streams. 

After they have been built, you will need to configure SIP account information in the SIP_LOGIN environment variable: 

    # export SIP_LOGIN=username:password@sip.example.com
Next, run slmodemd, passing the path to d-modem in the -e option.  Use -d<level> for debug logging. 

    # ./slmodemd/slmodemd -d9 -e ./d-modem
    SmartLink Soft Modem: version 2.9.11 Oct 28 2021 16:51:30 
    symbolic link `/dev/ttySL0' -> `/dev/pts/3' created. 
    modem `slamr0' created. TTY is `/dev/pts/3' 
    Use `/dev/ttySL0' as modem device, Ctrl+C for termination.

In another terminal, connect to the newly created serial device at 115200 bps: 

    # screen /dev/ttySL0 115200

You can now interact with this terminal (almost) as you would with a normal modem using standard AT commands.  A similar modem’s manual provides a more complete list. 

Because there isn’t any dial tone on our SIP connection, you’ll need to disable dial tone detection: 

    atx3 
    OK

To successfully connect, you will likely need to manually select a modulation and data rate.  In our testing, V.32bis (14.4kbps) and below appears to be the most reliable, though V.34 (33.6kbps) connections are sometimes successful.  For example, the following command selects V.32bis with a data rate of 4800 – 9600 bps.  Refer to the manual for further details. 

    at+ms=132,0,4800,9600 
    OK

Finally, dial the number of the target system.  Below shows a connection to the NIST atomic clock: 

    atd303-494-4774 
    CONNECT 9600 
    National Institute of Standards and Technology 
    Telephone Time Service, Generator 1b 
    Enter the question mark character for HELP 
                            D  L 
     MJD  YR MO DA HH MM SS ST S UT1 msADV         <OTM> 
    59515 21-10-28 21:40:18 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:19 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:20 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:21 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:22 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:23 11 0 -.1 045.0 UTC(NIST) *
 
## Known Issues / Future Work
- Connections are unreliable, and it is currently difficult to connect at speeds higher than 14.4kbps or so.  It might be possible to improve this by disabling/reconfiguring PJSIP’s jitter buffer. 
- Additional logging/error handling is needed 
- The serial interface could be replaced with stdio or a socket, and common AT configuration options could be exposed as command line options 
- There is currently no support for receiving calls 


## V.34 reliability

An optional handshake watchdog workaround is available for the exact i386
`dsplibs.o` shipped in this tree. It does not establish why a particular modem
path needs the workaround. The vendor threshold is its normal default, not a
packet-path calibration error. Other possible causes of `vpcm: Link Error`
remain; see [the investigation and subsequent corrections](https://github.com/strozfriedberg/D-Modem/issues/13).

On Linux, with the normal 32-bit build dependencies and GNU binutils installed:

```sh
./slmodemd/apply_watchdog_hook.sh ./slmodemd
# Use the separately built executable in place of ./slmodemd/slmodemd:
SLM_V34_LOWSIG=-1000000 ./slmodemd-watchdog/slmodemd -d2 -e ./d-modem /dev/slamr0
```

The installer checks the vendor object's SHA-256, builds in a temporary directory,
and verifies that the linked `vpcm_run` calls the wrapper and that the wrapper
reaches the original. It then publishes the separate `slmodemd-watchdog` directory;
it never edits the input tree. An optional second argument selects another output
directory. Repeating the command verifies an existing build; changed inputs or
output require a new directory. The hook adds no vendor object to the repository
and changes no vendor instructions. Other blob versions need a fresh ABI audit.

Without either variable below, the wrapper returns the original function's result
without reading or writing private DSP state or calling diagnostic getters:

| Variable | Effect |
| --- | --- |
| `SLM_V34_LOWSIG` | Override the handshake's signed 32-bit low-signal threshold before each fragment. `-1000000` disables this criterion because the metric is signed 16-bit. Unset or `1` preserves the original threshold. Invalid integers are rejected. |
| `SLM_V34_METRIC=N` | Log the observed signal metric, threshold, counter, and status every N fragments. Unset or zero disables logging. Fragment indices span the process lifetime. |

The independent acquisition/session timeouts remain. Experimental fatal-status,
rate-renegotiation, and retrain overrides are deliberately absent from this hook.
To return to vendor behavior, unset these variables or use the original binary.

Historical interleaved testing on one installation recorded 19/30 completed calls
with the original criterion and 27/30 with it disabled (two-sided Fisher exact
p = 0.030). Every completed call reported 33,600 bit/s **caller receive rate** in
both arms. That measures fewer failed handshakes, not faster negotiation, both
directional rates, a 100-call reliability result, or behavior on other hardware.
The [original methods and data](https://dialup.litenet.tel/research/v34-modem/)
include later corrections. RX noise and delayed self-echo proposed in issue #13
are separate experiments; this watchdog hook does not implement them.

Run the isolated wrapper checks with `python3 slmodemd/tests/test_watchdog.py`.
They verify argument/result passthrough, absence of default private-state access,
strict integer parsing, and threshold reapplication on a reused object. On Linux, also run `python3 slmodemd/tests/test_watchdog_install.py` for isolated
32-bit build, rerun, failure, and output-preservation checks. These are component
checks, not modem-call acceptance tests.

Copyright 2021 Aon plc
