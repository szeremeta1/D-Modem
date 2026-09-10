#!/usr/bin/env python3
"""Build experimental V.90 negotiation gates in a separate D-Modem tree.

Linux: python3 slmodemd/v90-gates/build.py slmodemd /tmp/new-gate-tree
The exact public i386 DSP object is required. Never installs or starts a modem.
Run the resulting offline-check separately; it has aborting device stubs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

EXPECTED = "1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379"
SYMBOLS = {
    "V8Create": (0x73e20, 0x464),
    "initTxSequence": (0x75860, 0x3e9),
    "rebuildJMSequence": (0x75c50, 0xc3e),
    "V8UpdateModemParameters": (0x74840, 0x412),
    "VPCMXF_Create": (0xfcf0, 0x1ef),
    "VPcmV34Create": (0xaa70, 0x948),
    "VPCMXF_SessionTermination": (0xf730, 0x13),
}
TEST_SYMBOLS = {"vpcm_create": (0x3a00, 0x3c9), "vpcm_delete": (0x3dd0, 0x6e)}
RELOCS = {
    0x369d: "V8Create", 0x3858: "V8UpdateModemParameters",
    0x76e91: "initTxSequence", 0x7716f: "initTxSequence",
    0x782f3: "rebuildJMSequence", 0x3afe: "VPCMXF_Create",
    0x3c71: "VPcmV34Create", 0x3dff: "VPCMXF_SessionTermination",
}
# Public host objects only. This test executable substitutes main; the actual
# slmodemd executable is linked by the source directory's stock Makefile.
OBJECTS = "modem_cmdline modem modem_datafile modem_at modem_timer modem_pack modem_ec modem_comp modem_param modem_debug homolog_data dp_sinus dp_dummy v90_gate dsplibs sysdep_common".split()


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    base = args.source.resolve()
    if args.output.is_symlink():
        parser.error("output must not be a symlink")
    dest = args.output.resolve()
    here = Path(__file__).resolve().parent
    if dest.exists() or dest == base or base in dest.parents:
        parser.error("output must be new and outside the source tree")
    if not dest.parent.is_dir():
        parser.error("output parent must already exist")
    obj = base / "dsplibs.o"
    if not obj.is_file() or sha(obj) != EXPECTED:
        parser.error("unknown DSP object: refuse; revalidate ABI")
    nm = {}
    for line in run("nm", "-S", obj).splitlines():
        fields = line.split()
        if len(fields) == 4:
            nm[fields[3]] = (int(fields[0], 16), int(fields[1], 16), fields[2])
    for name, pair in SYMBOLS.items():
        if nm.get(name) != (*pair, "T"):
            parser.error(f"unexpected symbol: {name} {nm.get(name)}")
    for name, pair in TEST_SYMBOLS.items():
        if nm.get(name) != (*pair, "t"):
            parser.error(f"unexpected test alias: {name} {nm.get(name)}")
    dis = run("objdump", "-drw", obj)
    for at, name in RELOCS.items():
        if not re.search(rf"\b{at:x}: R_386_PC32\s+{name}(?:\s|$)", dis):
            parser.error(f"expected symbolic relocation absent: {at:x} {name}")
    section = next(line.split() for line in run("readelf", "-S", obj).splitlines()
                   if "] .text" in line and "PROGBITS" in line)
    i = section.index(".text")
    addr, offset = int(section[i+2], 16), int(section[i+3], 16)
    data = bytearray(obj.read_bytes())
    at = offset + 0x3607 - addr
    old, new = bytes.fromhex("0f85b3000000"), bytes.fromhex("e9b400000090")
    if data[at:at+6] != old:
        parser.error("V8 offer branch mismatch")
    data[at:at+6] = new
    host_source = (base / "modem.c").read_text()
    host_anchor = "\t\top = get_dp_operations(id);"
    if host_source.count(host_anchor) != 1 or "dmodem_v90_host_transition" in host_source:
        parser.error("host datapump transition anchor mismatch")
    host_source = "extern void dmodem_v90_host_transition(unsigned, unsigned, unsigned);\n" + host_source.replace(
        host_anchor, "\t\tdmodem_v90_host_transition(dp_id, m->caller, id);\n" + host_anchor)
    makefile = (base / "Makefile").read_text()
    anchor = "$(dp-objs) dsplibs.o $(sysdep-objs)"
    if makefile.count(anchor) != 1:
        parser.error("public Makefile link anchor mismatch")
    makefile = makefile.replace(anchor, "$(dp-objs) v90_gate.o dsplibs.o $(sysdep-objs)")
    makefile += "\n# Experimental negotiation gate constructors use libm.\nLFLAGS += -lm\n"
    stage = Path(tempfile.mkdtemp(prefix=".dmodem-v90-gates-", dir=dest.parent))
    try:
        shutil.copytree(base, stage, dirs_exist_ok=True)
        obj = stage / "dsplibs.o"
        obj.write_bytes(data)
        command = ["objcopy"]
        for name, (address, _) in SYMBOLS.items():
            command += [f"--weaken-symbol={name}",
                        f"--add-symbol={name}__gate_original=.text:0x{address:x},global,function"]
        for name, (address, _) in TEST_SYMBOLS.items():
            command += [f"--add-symbol=v90_test_{name}=.text:0x{address:x},global,function"]
        subprocess.run(command + [str(obj)], check=True)
        for name in ("v90_gate.c", "v90_gate_abi.h"):
            shutil.copyfile(here / name, stage / name)
        (stage / "Makefile").write_text(makefile)
        (stage / "modem.c").write_text(host_source)
        for old_obj in stage.glob("*.o"):
            if old_obj.name != "dsplibs.o":
                old_obj.unlink()
        for filename in ("slmodemd", "modem_test", ".build_profile", ".depend"):
            (stage / filename).unlink(missing_ok=True)
        subprocess.run(["make", "-s", "slmodemd"], cwd=stage, check=True)
        binary_dis = run("objdump", "-d", stage / "slmodemd")
        if not re.search(r"\bcall\s+[^\n]*<dmodem_v90_host_transition>", binary_dis):
            raise RuntimeError("no verified host mode gate call")
        for name in SYMBOLS:
            for target in (name, name + "__gate_original"):
                if not re.search(rf"\bcall\s+[^\n]*<{target}>", binary_dis):
                    raise RuntimeError(f"no verified call to {target}")
        (stage / "gate-link-disassembly.txt").write_text(binary_dis)
        subprocess.run(["gcc", "-m32", "-no-pie", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(stage), "-I" + str(here), "-o", str(stage / "offline-check"),
                        str(here / "offline_check.c"),
                        *[str(stage / (name + ".o")) for name in OBJECTS], "-lm"], check=True)
        if sha(base / "dsplibs.o") != EXPECTED:
            raise RuntimeError("source DSP object changed during build")
        report = {"source_sha256": EXPECTED, "binary_sha256": sha(stage / "slmodemd"),
                  "patched_object_sha256": sha(obj), "output": str(dest),
                  "wrapper_sha256": sha(here / "v90_gate.c"),
                  "abi_header_sha256": sha(here / "v90_gate_abi.h"),
                  "source_host_sha256": sha(base / "modem.c"),
                  "conditioned_host_sha256": sha(stage / "modem.c"),
                  "offline_binary_sha256": sha(stage / "offline-check"),
                  "host_transition_verified": True,
                  "call_targets_verified": list(SYMBOLS),
                  "warning": "Negotiation experiment only; native 8k PCM transport not integrated"}
        (stage / "gate-build.json").write_text(json.dumps(report, indent=2) + "\n")
        # GNU mv -n may return success for an existing destination; source
        # existence is the independent check that publication really happened.
        moved = subprocess.run(["mv", "-T", "-n", "--", str(stage), str(dest)])
        if moved.returncode or stage.exists():
            raise RuntimeError("output appeared during build or publication failed")
        print(json.dumps(report, indent=2))
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == "__main__":
    main()
