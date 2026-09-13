#!/usr/bin/env python3
"""Run only the three hashed offline probes and preserve a safe result summary."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run_probe(args, env):
    def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env=env, stdin=subprocess.DEVNULL, close_fds=True,
                            preexec_fn=no_core, start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=15)
    except BaseException:
        # Kill only this test's separate process group, including a tracee if
        # strace is interrupted. Never leave a timed-out constructor running.
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        proc.communicate(timeout=5)
        raise
    return subprocess.CompletedProcess(args, proc.returncode, stdout, stderr)


# Deliberately bounded observation, not a syscall sandbox. Missing/ambiguous
# trace records must not become zero-call evidence. -qq suppresses exit notices;
# unfinished/resumed calls, signals and unexpected formats fail closed here.
TRACE_FILTER = "network,open,openat,openat2,creat"
OPEN_CALLS = ("open", "openat", "openat2", "creat")


def trace_summary(raw):
    lines = [line for line in raw.splitlines() if line.strip()]
    if not lines:
        raise ValueError("Missing offline syscall trace evidence")
    files = []
    for line in lines:
        call = re.fullmatch(r"(?:[1-9]\d*\s+)?(\w+)\((.*)\)\s+=\s+(-?\d+)(?:\s+[A-Z][A-Z0-9_]*\s+\([^\n]*\))?", line)
        if call is None:
            raise ValueError("Unparsed or incomplete offline syscall trace record")
        name, args, result = call.groups()
        if name not in OPEN_CALLS:
            raise ValueError("An offline probe attempted a network or unexpected syscall")
        # Parse the pathname at its exact ABI position. C-only/octal escapes,
        # unfinished records and ambiguous relative names are not accepted.
        prefix = r'(?:AT_FDCWD|-?\d+),\s*' if name in ("openat", "openat2") else ""
        path_match = re.match(prefix + r'("(?:[^"\\]|\\.)*")\s*,', args)
        if path_match is None:
            raise ValueError("Unparsed offline open pathname")
        try:
            path = json.loads(path_match.group(1))
        except (ValueError, TypeError) as error:
            raise ValueError("Ambiguous offline open pathname escape") from error
        if not path.startswith("/") or "\0" in path:
            raise ValueError("Ambiguous relative or NUL-containing offline open pathname")
        normalized = "/" + os.path.normpath(path).lstrip("/")
        if normalized == "/dev" or normalized.startswith("/dev/"):
            raise ValueError("An offline probe attempted a device-path open")
        files.append(path)
    return {"network_calls": 0, "device_opens": 0, "open_count": len(files),
            "parsed_records": len(lines), "trace_filter": TRACE_FILTER,
            "opened_basenames": sorted({Path(p).name for p in files}),
            "scope": "Only selected network and pathname-opening syscalls observed; no claim about every possible device-access mechanism or path alias."}


def result_summary(name, raw):
    if name == "tx-offline":
        rows = re.findall(r"generated=(\d+) nonzero=(\d+) exact_g711_roundtrip_mismatches=(\d+) phase=(\d+) p3=(\d+)", raw)
        if rows != [("20000", "19856", "0", "1", "3")] * 3 or "OFFLINE TX ABI CHECKS PASS:" not in raw:
            raise ValueError("Native generator assertions or measured vector changed")
        return {"constructions": 3, "native_samples_per_vector": 20000, "nonzero_per_vector": 19856,
                "g711_roundtrip_mismatches": 0, "chunk_sizes": [1, 40, 40],
                "common_dispatch_checked": True, "waiting_in_jd": True, "received_waveform": False}
    if name == "ja-offline":
        rows = re.findall(r"PASS N=(\d+) LSP=(\d+) LTP=(\d+) bits=(\d+) bad_single_bits=(\d+) all_truncations=MORE", raw)
        expected = [("0","1","1","240","240"), ("1","1","1","256","256"),
                    ("3","17","31","308","308"), ("128","128","128","1566","1566"),
                    ("255","128","128","2654","2654")]
        if rows != expected or "PASS 16384 descriptor boundary combinations" not in raw or "JA FRAME CHECKS PASS:" not in raw:
            raise ValueError("Framed-Ja parser assertions or matrix changed")
        return {"descriptor_combinations": 16384, "exhaustive_representative_frames": 5,
                "single_bit_changes_not_accepted": 5024, "truncated_prefixes_not_accepted": 5024,
                "already_descrambled_and_aligned": True, "received_waveform": False}
    if name == "ja-control-offline":
        if not re.search(r"PASS decoded-Ja event: 308 bits, .*duplicate blocked; 40 native symbols \(27 nonzero\)", raw) or "OFFLINE ONLY:" not in raw:
            raise ValueError("Decoded-Ja control assertions or output changed")
        return {"generated_ja_bits": 308, "native_samples": 40, "nonzero_samples": 27,
                "constructor_pointers_preserved": True, "invalid_input_does_not_commit": True,
                "duplicate_after_phase3_rejected": True, "received_waveform": False}
    raise ValueError("Unknown offline probe")


def validate(output):
    build = json.loads((output/"build.json").read_text())
    names = ("tx-offline", "ja-offline", "ja-control-offline")
    if set(build["binary_sha256"]) != set(names):
        raise ValueError("Unexpected build probe inventory")
    if (output/"validation.json").exists() or any((output/(name+suffix)).exists() for name in names for suffix in (".stdout.raw", ".stderr.raw", ".trace.raw")):
        raise ValueError("Validation output already exists; use a fresh build directory")
    for name in names:
        if (output/name).is_symlink() or digest(output/name) != build["binary_sha256"][name]:
            raise ValueError("Probe binary differs from build manifest")
    records = {}
    for name in names:
        trace = output/(name+".trace.raw")
        env = {"PATH": os.defpath, "LC_ALL": "C"}
        run = run_probe(["strace", "-f", "-qq", "-e", "trace=" + TRACE_FILTER, "-o", str(trace), str(output/name)], env)
        (output/(name+".stdout.raw")).write_text(run.stdout)
        (output/(name+".stderr.raw")).write_text(run.stderr)
        if run.returncode:
            raise ValueError("Offline probe failed; inspect its private raw files: " + name)
        if digest(output/name) != build["binary_sha256"][name]:
            raise ValueError("Probe executable changed during validation")
        records[name] = {"binary_sha256": build["binary_sha256"][name], "exit_status": run.returncode,
                         "result": result_summary(name, run.stdout), "trace": trace_summary(trace.read_text()),
                         "stdout_sha256": digest(output/(name+".stdout.raw")),
                         "stderr_sha256": digest(output/(name+".stderr.raw")), "trace_sha256": digest(trace)}
    report = {"schema": 1, "validated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "build_manifest_sha256": digest(output/"build.json"), "probes": records,
              "strace": subprocess.check_output(["strace", "--version"], text=True).splitlines()[0],
              "run_environment": {"LC_ALL": "C", "PATH": "system default; no inherited modem controls"},
              "timeout_seconds_per_probe": 15, "core_dumps_disabled": True,
              "stdin": "devnull", "stdout_stderr": "private pipes", "other_inherited_fds_closed": True,
              "syscall_observation_scope": TRACE_FILTER, "syscall_filter_is_not_a_sandbox": True,
              "physical_calls": 0, "waveform_receiver_tested": False, "transport_tested": False,
              "scope": "Synthetic local constructors, packed/descrambled Ja bits and local native sample generation only; no peer negotiation or end-to-end rate/reliability claim."}
    (output/"validation.json").write_text(json.dumps(report, indent=2)+"\n")
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    a = p.parse_args()
    print(json.dumps(validate(a.output.absolute()), indent=2))
