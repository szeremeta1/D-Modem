#!/usr/bin/env python3
"""Rebuild offline native PCM probes using only pinned public D-Modem sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROBES = {"tx-offline": "tx_offline.c", "ja-offline": "ja_offline.c",
          "ja-control-offline": "ja_control_offline.c"}
FLAGS = ["-m32", "-no-pie", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(path, expected):
    if path.is_symlink() or not path.is_file() or sha(path) != expected:
        raise ValueError("Pinned regular-file input differs: " + path.name)


def inventory(source, output, here=HERE):
    if source.is_symlink() or not source.is_dir():
        raise ValueError("Regular public source directory required")
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError("New output with an existing parent required")
    for root in (source.resolve(), here.resolve()):
        dest = output.resolve()
        if dest == root or root in dest.parents or dest in root.parents:
            raise ValueError("Output must be outside the input trees")
    abi = json.loads((here / "PUBLIC-ABI.json").read_text())
    inputs = {source / "dsplibs.o": abi["public_dsp_sha256"]}
    for key, root in (("public_source_sha256", source),
                      ("public_gate_source_sha256", source / "v90-gates"),
                      ("probe_source_sha256", here)):
        for name, digest in abi[key].items():
            if Path(name).name != name:
                raise ValueError("Input inventory must contain only basenames")
            inputs[root / name] = digest
    for path, expected in inputs.items():
        checked(path, expected)
    return abi, inputs


def run_owned(command, log, timeout=180):
    # No inherited MAKEFLAGS, compiler settings, preload or modem controls.
    env = {"PATH": os.defpath, "LC_ALL": "C"}
    with log.open("xb") as stream:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream,
                                stderr=subprocess.STDOUT, env=env, close_fds=True,
                                start_new_session=True)
        try:
            code = proc.wait(timeout=timeout)
        except BaseException:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
            raise
        if code:
            raise ValueError("Build command failed; inspect " + log.name)


def stage_public(inputs, source, public):
    public.mkdir(mode=0o700)
    (public / "v90-gates").mkdir(mode=0o700)
    for path, expected in inputs.items():
        if path.parent in (source, source / "v90-gates"):
            dest = public / path.relative_to(source)
            dest.write_bytes(path.read_bytes())
            checked(dest, expected)


def build(source, output):
    if platform.system() != "Linux":
        raise ValueError("Linux with an i386-capable GCC/libc toolchain required")
    abi, inputs = inventory(source, output)
    tools = {name: sha(HERE / name) for name in
             ("build.py", "validate.py", "test_build.py", "PUBLIC-ABI.json")}
    os.umask(0o077)
    output.mkdir(mode=0o700)
    public = output / "public-source"
    # Copy an explicit public inventory, never a user's working tree, objects,
    # hidden Make includes, arbitrary extra sources or private vendor hooks.
    stage_public(inputs, source, public)
    gate = output / "gate"
    run_owned([sys.executable, str(public / "v90-gates/build.py"),
               str(public), str(gate)], output / "gate-build.log")
    gate_report = json.loads((gate / "gate-build.json").read_text())
    if not gate_report.get("host_transition_verified"):
        raise ValueError("Public negotiation gate verification absent")
    objects = {name + ".o": sha(gate / (name + ".o"))
               for name in abi["required_public_objects"]}
    nm = {}
    for line in subprocess.check_output(["nm", "-S", str(gate / "dsplibs.o")],
                                        text=True, env={"PATH": os.defpath, "LC_ALL": "C"}).splitlines():
        fields = line.split()
        if len(fields) == 4:
            nm[fields[3]] = (int(fields[0], 16), int(fields[1], 16), fields[2])
    for symbol, shape in abi["native_symbols"].items():
        if nm.get(symbol) != (shape["address"], shape["size"], "T"):
            raise ValueError("Native symbol address/size differs: " + symbol)
    binaries = {}
    for binary, name in PROBES.items():
        run_owned(["gcc", *FLAGS, "-I" + str(gate), "-o", str(output / binary),
                   str(HERE / name), *[str(gate / name) for name in objects], "-lm"],
                  output / (binary + "-link.log"), timeout=90)
        binaries[binary] = sha(output / binary)
    for path, expected in inputs.items():
        checked(path, expected)
    for name, expected in tools.items():
        checked(HERE / name, expected)
    for name, expected in objects.items():
        checked(gate / name, expected)
    report = {"schema": 1, "public_gate_commit": abi["reviewed_gate_commit"],
              "abi_inventory_sha256": tools["PUBLIC-ABI.json"],
              "tool_source_sha256": tools, "source_sha256": abi["probe_source_sha256"],
              "original_dsp_sha256": abi["public_dsp_sha256"],
              "object_sha256": objects, "binary_sha256": binaries,
              "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
              "linker": subprocess.check_output(["ld", "--version"], text=True).splitlines()[0],
              "architecture": platform.machine(), "flags": FLAGS + ["-I<GATE>", "-lm"],
              "inputs_unchanged": True, "private_objects_used": False,
              "probes_executed": False, "physical_calls": 0,
              "scope": "Public-source offline probe link only; no modem run loop, device or peer."}
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="public slmodemd directory")
    parser.add_argument("output", type=Path, help="new private output directory")
    args = parser.parse_args()
    print(json.dumps(build(args.source.absolute(), args.output.absolute()), indent=2))
