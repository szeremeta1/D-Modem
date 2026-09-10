#!/usr/bin/env python3
"""Build an exact-object near-EC output experiment from a prebuilt baseline.

Linux: python3 build.py BASELINE NEW_OUTPUT
Reuses every baseline object; never invokes make or rebuilds host source.
No service, device, replay, or modem process is started.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

EXPECTED = "1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379"
OBJECTS = tuple(n + ".o" for n in (
    "modem_main modem_cmdline modem modem_datafile modem_at modem_timer modem_pack "
    "modem_ec modem_comp modem_param modem_debug homolog_data dp_sinus dp_dummy "
    "dsplibs sysdep_common").split())
SYMBOLS = {"V34EchoFilter": (0x71ec0, 0x8b),
           "modem_serrint": (0x5cf80, 0x63e), "adaptecho": (0x5d940, 0x2f3),
           "V34InitializeImplementationSpecific": (0x71d70, 0xb5),
           "V34EchoCleanUp": (0x71e30, 0x53), "v34FreezeEcho": (0x5e200, 0xde)}
RETURNS = {"dmodem_near_data_return": 0x5d41e,
           "dmodem_near_training_return": 0x5d9cf,
           "dmodem_far_data_return": 0x5d4f9}


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbols(text):
    result = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            result[parts[2]] = (int(parts[0], 16), parts[1])
    return result


def nm(path):
    return symbols(run("nm", "-g", "--defined-only", path))


def near_bindings(syms, disassembly):
    required = ("V34EchoFilter", "V34EchoFilter__near_original", *RETURNS)
    if any(name not in syms for name in required):
        raise ValueError("Near-EC symbol contract is incomplete")
    wrapper, original = syms[required[0]], syms[required[1]]
    if wrapper[1] != "T" or original[1] != "T" or wrapper[0] == original[0]:
        raise ValueError("Near-EC wrapper must be a distinct strong text symbol")
    calls = {}
    for line in disassembly.splitlines():
        match = re.fullmatch(r"\s*([0-9a-fA-F]+):\s+e8\s+(?:[0-9a-fA-F]{2}\s+){4}call\s+"
                             r"([0-9a-fA-F]+)\s+<([^>]+)>\s*", line)
        if match:
            calls[int(match[1], 16)] = (int(match[2], 16), match[3])
    for marker in RETURNS:
        if calls.get(syms[marker][0] - 5) != (wrapper[0], "V34EchoFilter"):
            raise ValueError("Caller not linked through near-EC wrapper: " + marker)
    body = re.search(r"^[0-9a-fA-F]+ <V34EchoFilter>:\n(.*?)(?=^[0-9a-fA-F]+ <|\Z)",
                     disassembly, re.M | re.S)
    if not body or not re.search(r"\bcall\s+0*" + format(original[0], "x") +
                                 r"\s+<V34EchoFilter__near_original>", body[1]):
        raise ValueError("Near-EC wrapper does not call the original vendor filter")
    return {name: hex(syms[name][0]) for name in RETURNS}


def elf32(data):
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 3:
        raise ValueError("Expected little-endian i386 ELF")
    return struct.unpack_from("<H", data, 16)[0]


def allocated_sections(data):
    elf32(data)
    offset = struct.unpack_from("<I", data, 0x20)[0]
    size, count, index = struct.unpack_from("<HHH", data, 0x2e)
    headers = [struct.unpack_from("<10I", data, offset + i * size) for i in range(count)]
    names_header = headers[index]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    return {names[h[0]:].split(b"\0", 1)[0].decode(): data[h[4]:h[4] + h[5]]
            for h in headers if h[2] & 2 and h[1] != 8}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    base = args.baseline.resolve()
    if args.output.is_symlink():
        parser.error("output must not be a symlink")
    dest = args.output.resolve()
    here = Path(__file__).resolve().parent
    if dest.exists() or dest == base or base in dest.parents:
        parser.error("output must be new and outside baseline")
    if not dest.parent.is_dir():
        parser.error("output parent must already exist")
    for name in (*OBJECTS, "slmodemd"):
        path = base / name
        if path.is_symlink() or not path.is_file() or not os.access(path, os.R_OK):
            parser.error("baseline needs a readable prebuilt regular file: " + name)
    original = base / "dsplibs.o"
    if sha(original) != EXPECTED:
        parser.error("unknown DSP: refuse; revalidate ABI")
    sized = {}
    for line in run("nm", "-S", original).splitlines():
        p = line.split()
        if len(p) == 4:
            sized[p[3]] = (int(p[0], 16), int(p[1], 16), p[2])
    for name, pair in SYMBOLS.items():
        if sized.get(name) != (*pair, "T"):
            parser.error(f"unexpected symbol {name}: {sized.get(name)}")
    dis = run("objdump", "-drw", original)
    calls = [int(x, 16) for x in re.findall(r"\b([0-9a-f]+): R_386_PC32\s+V34EchoFilter(?:\s|$)", dis)]
    if sorted(calls) != [0x5d41a, 0x5d4f5, 0x5d9cb]:
        parser.error(f"changed echo caller set: {calls}")
    for name in OBJECTS:
        if name != "dsplibs.o" and "V34EchoFilter" in nm(base / name):
            parser.error("competing echo wrapper in baseline object: " + name)
    input_hashes = {name: sha(base / name) for name in (*OBJECTS, "slmodemd")}
    source_hash = sha(here / "near_ec.c")
    original_sections = allocated_sections(original.read_bytes())
    kind = elf32((base / "slmodemd").read_bytes())
    if kind not in (2, 3):
        parser.error("baseline executable is neither EXEC nor DYN")
    link_mode = "-pie" if kind == 3 else "-no-pie"
    stage = Path(tempfile.mkdtemp(prefix=".dmodem-near-ec-", dir=dest.parent))
    try:
        shutil.copytree(base, stage, dirs_exist_ok=True)
        obj = stage / "dsplibs.o"
        command = ["objcopy", "--weaken-symbol=V34EchoFilter",
                   "--add-symbol=V34EchoFilter__near_original=.text:0x71ec0,global,function"]
        command += [f"--add-symbol={name}=.text:0x{address:x},global" for name, address in RETURNS.items()]
        subprocess.run(command + [str(obj)], check=True)
        if allocated_sections(obj.read_bytes()) != original_sections:
            raise ValueError("DSP instruction/data section bytes changed")
        dsp_symbols = nm(obj)
        if dsp_symbols.get("V34EchoFilter") != (0x71ec0, "W") or \
                dsp_symbols.get("V34EchoFilter__near_original") != (0x71ec0, "T"):
            raise ValueError("vendor weak definition/original alias mismatch")
        shutil.copyfile(here / "near_ec.c", stage / "near_ec.c")
        subprocess.run(["gcc", "-m32", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                        "-c", "near_ec.c", "-o", "near_ec.o"], cwd=stage, check=True)
        subprocess.run(["gcc", "-m32", link_mode, "-o", "slmodemd", *OBJECTS, "near_ec.o", "-lm"],
                       cwd=stage, check=True)
        linked = run("objdump", "-d", stage / "slmodemd")
        markers = near_bindings(nm(stage / "slmodemd"), linked)
        (stage / "near-ec-link-disassembly.txt").write_text(linked)
        for name, flag in (("offline-check", "-no-pie"), ("offline-check-pie", "-pie")):
            subprocess.run(["gcc", "-m32", flag, "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                            "-o", str(stage / name), str(here / "offline_check.c"),
                            *[str(stage / n) for n in OBJECTS if n != "modem_main.o"],
                            str(stage / "near_ec.o"), "-lm"], check=True)
            near_bindings(nm(stage / name), run("objdump", "-d", stage / name))
        for name, digest in input_hashes.items():
            if sha(base / name) != digest or (name not in ("dsplibs.o", "slmodemd") and sha(stage / name) != digest):
                raise ValueError("baseline/reused object changed during build: " + name)
        if sha(stage / "near_ec.c") != source_hash or sha(here / "near_ec.c") != source_hash:
            raise ValueError("interposer source changed during build")
        report = {"source_sha256": EXPECTED, "binary_sha256": sha(stage / "slmodemd"),
                  "object_sha256": sha(obj), "output": str(dest), "link_mode": link_mode,
                  "wrapper_source_sha256": source_hash, "reused_input_sha256": input_hashes,
                  "allocated_sections_unchanged": sorted(original_sections),
                  "return_markers": markers, "wrapper_local_delegation_verified": True,
                  "change": "symbol-only near-context output bypass, strict startup opt-in",
                  "warning": "offline function checks only; no modem outcome established"}
        (stage / "near-ec-build.json").write_text(json.dumps(report, indent=2) + "\n")
        moved = subprocess.run(["mv", "-T", "-n", "--", str(stage), str(dest)])
        if moved.returncode or stage.exists():
            raise RuntimeError("output appeared during build or publication failed")
        print(json.dumps(report, indent=2))
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == "__main__":
    main()
