#!/usr/bin/env python3
"""Build a separate, default-disabled socket-I/O experiment; never invoke Make.

Usage: build.py BASE_WITH_PREBUILT_OBJECTS NEW_OUTPUT_DIRECTORY
Only modem_main.c/modem_main.o and the final executable change in the copy.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

COMMON = "modem_cmdline modem modem_datafile modem_at modem_timer modem_pack modem_ec modem_comp modem_param modem_debug homolog_data dp_sinus dp_dummy dsplibs sysdep_common".split()
PROFILES = {
    "public-stock": {
        "source": "8fe87bbaccdcc989e7eba4017015dac07420d478a569ec3bbc0e88c95ec2b688",
        "host_object": "889d6bf23cf5c92af2d52855ed1f6c5604fe6906f2a27f9578ea46deeb02e302",
        "dsp": "1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379",
        "binary": "59e2c8816b4784d1e9ac0c8a943d0a272b1a89c6ed865b7b12f148010c7c7381",
        "objects": COMMON,
    },

}


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def once(source, old, new):
    if source.count(old) != 1:
        raise ValueError("Source anchor count differs: " + repr(old[:90]))
    return source.replace(old, new)


def patch_source(source):
    source = once(source, "#include <modem_debug.h>", '#include <modem_debug.h>\n'
                  '#include "dmodem_socket_io.h"\n'
                  'static sio_state socket_io;\n'
                  'static struct device_struct *socket_io_device;\n'
                  'static int socket_io_cancelled(void);\n'
                  '#define SOCKET_IO_ACTIVE(d) (socket_io.enabled && socket_io_device == (d))')
    source = once(source, 'static volatile sig_atomic_t keep_running = 1;',
                  'static volatile sig_atomic_t keep_running = 1;\n'
                  'static int socket_io_cancelled(void) { return !keep_running; }')
    source = once(source, '\tDBG("socket_start...\\n");',
                  '\tDBG("socket_start...\\n");\n'
                  '\tsocket_io_device = NULL;\n'
                  '\tif (sio_init(&socket_io, getenv("DMODEM_SOCKET_IO"), socket_io_cancelled) < 0) {\n'
                  '\t\tERR("socket_io: INSTRUMENT ERROR invalid DMODEM_SOCKET_IO (expected 0 or 1)\\n");\n'
                  '\t\tsio_report(&socket_io);\n'
                  '\t\treturn -EINVAL;\n'
                  '\t}\n'
                  '\tif (socket_io.enabled) socket_io_device = dev;')
    source = once(source, '\tDBG("socket_stop...\\n");',
                  '\tDBG("socket_stop...\\n");\n'
                  '\tif (SOCKET_IO_ACTIVE(dev)) {\n'
                  '\t\tsio_report(&socket_io);\n'
                  '\t\tsocket_io.enabled = socket_io.pending = 0;\n'
                  '\t\tsocket_io_device = NULL;\n'
                  '\t}')
    source = once(source, '\t\tret = write(dev->fd, outbuf, ret);',
                  '\t\tif (SOCKET_IO_ACTIVE(dev)) {\n'
                  '\t\t\tint n = sio_write(&socket_io, dev->fd, outbuf, ret/2);\n'
                  '\t\t\tret = n < 0 ? n : n*2;\n'
                  '\t\t} else ret = write(dev->fd, outbuf, ret);')
    source = once(source, '\tint ret = read(dev->fd, buf, size*2);',
                  '\tint ret;\n'
                  '\tif (SOCKET_IO_ACTIVE(dev)) return sio_read(&socket_io, dev->fd, buf, size);\n'
                  '\tret = read(dev->fd, buf, size*2);')
    source = once(source, '\tint ret = write(dev->fd, buf, size*2);',
                  '\tint ret;\n'
                  '\tif (SOCKET_IO_ACTIVE(dev)) return sio_write(&socket_io, dev->fd, buf, size);\n'
                  '\tret = write(dev->fd, buf, size*2);')
    source = once(source, '\t\t\tif(m->update_delay < 0) {',
                  '\t\t\tif (SOCKET_IO_ACTIVE(dev) && m->update_delay < 0) {\n'
                  '\t\t\t\tif (sio_skip(&socket_io, &in, &count, &m->update_delay, &dev->delay) < 0) {\n'
                  '\t\t\t\t\tERR("socket_io: INSTRUMENT ERROR invalid input delay adjustment\\n");\n'
                  '\t\t\t\t\tsio_report(&socket_io); return -1;\n'
                  '\t\t\t\t}\n'
                  '\t\t\t\tif (count == 0) continue;\n'
                  '\t\t\t}\n'
                  '\t\t\telse if(m->update_delay < 0) {')
    source = once(source, '\t\t\tmodem_process(m,inbuf,outbuf,count);',
                  '\t\t\tmodem_process(m, SOCKET_IO_ACTIVE(dev) ? in : inbuf, outbuf, count);')
    source = once(source, '\t\t\tcount = device_write(dev,outbuf,count);',
                  '\t\t\tif (SOCKET_IO_ACTIVE(dev) && m->update_delay > 0 &&\n'
                  '\t\t\t    sio_padding(&socket_io, m->update_delay, sizeof(outbuf), dev->delay) < 0) {\n'
                  '\t\t\t\tERR("socket_io: INSTRUMENT ERROR invalid output delay before block write\\n");\n'
                  '\t\t\t\tsio_report(&socket_io); return -1;\n'
                  '\t\t\t}\n'
                  '\t\t\tcount = device_write(dev,outbuf,count);')
    source = once(source, '\t\t\t\tmemset(outbuf, 0, m->update_delay*2);',
                  '\t\t\t\tif (SOCKET_IO_ACTIVE(dev) &&\n'
                  '\t\t\t\t    sio_padding(&socket_io, m->update_delay, sizeof(outbuf), dev->delay) < 0) {\n'
                  '\t\t\t\t\tERR("socket_io: INSTRUMENT ERROR invalid output delay padding\\n");\n'
                  '\t\t\t\t\tsio_report(&socket_io); return -1;\n'
                  '\t\t\t\t}\n'
                  '\t\t\t\tmemset(outbuf, 0, m->update_delay*2);')
    return source


def elf32_i386(path, expected_type):
    h = subprocess.check_output(["readelf", "-h", str(path)], text=True)
    if not ("ELF32" in h and "Intel 80386" in h and
            re.search(r"Type:\s+" + expected_type + r"\b", h)):
        raise ValueError("Unexpected ELF ABI/type: " + str(path))


def code_evidence(path):
    """Allocated bytes and symbolic relocations, excluding path-bearing DWARF."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01": raise ValueError("Not little-endian ELF32")
    shoff = struct.unpack_from("<I", data, 32)[0]
    entsize, number, strings = struct.unpack_from("<HHH", data, 46)
    sections = [struct.unpack_from("<10I", data, shoff+i*entsize) for i in range(number)]
    st = sections[strings]
    names = data[st[4]:st[4]+st[5]]
    result = {}
    for row in sections:
        if not row[2] & 2: continue
        name = names[row[0]:names.index(b"\0", row[0])].decode()
        body = b"" if row[1] == 8 else data[row[4]:row[4]+row[5]]
        result[name] = {"type": row[1], "flags": row[2], "size": row[5],
                        "sha256": hashlib.sha256(body).hexdigest(), "relocations": []}
    target = None
    for line in subprocess.check_output(["objdump", "-r", str(path)], text=True).splitlines():
        match = re.fullmatch(r"RELOCATION RECORDS FOR \[(.+)\]:", line)
        if match: target = match[1]
        elif target in result and re.match(r"^[0-9a-fA-F]+\s+R_", line):
            result[target]["relocations"].append(line.split())
    return result


def main():
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    base, out = (Path(v).resolve() for v in sys.argv[1:])
    here = Path(__file__).resolve().parent
    if out.exists() or base == out or base in out.parents or out in base.parents:
        raise SystemExit("Output must be new and separate from base")
    matches = [(n, p) for n, p in PROFILES.items()
               if sha(base / "modem_main.c") == p["source"]]
    if len(matches) != 1: raise SystemExit("Unreviewed host source hash")
    name, profile = matches[0]
    for file, field in [("modem_main.o", "host_object"), ("dsplibs.o", "dsp"), ("slmodemd", "binary")]:
        if sha(base / file) != profile[field]: raise SystemExit("Unreviewed baseline " + file)
    objects = [n + ".o" for n in profile["objects"]]
    for file in ["modem_main.c", "modem_main.o", "slmodemd", *objects]:
        if (base / file).is_symlink(): raise SystemExit("Refusing linked input " + file)
    elf32_i386(base / "slmodemd", "DYN")
    for file in ["modem_main.o", *objects]: elf32_i386(base / file, "REL")
    # Copy only the checked link inputs plus local public headers. No unrelated
    # files, hidden state, Makefiles, daemons, or credentials are copied.
    files = ["modem_main.c", "modem_main.o", "slmodemd", *objects]
    files += [p.name for p in base.glob("*.h") if not p.is_symlink()]
    inputs = {n: sha(base/n) for n in files}
    source = patch_source((base / "modem_main.c").read_text())
    header = here / "dmodem_socket_io.h"
    header_sha = sha(header)
    out.mkdir(parents=True)
    for file in files: shutil.copyfile(base/file, out/file)
    # Pin the full historical link, not just a plausible set of old objects.
    # The untouched object set must reproduce the original executable exactly.
    baseline_link = out / "pristine-relinked-slmodemd"
    subprocess.run(["gcc", "-m32", "-pie", "-o", str(baseline_link),
                    "modem_main.o", *objects, "-lm"], cwd=out, check=True)
    if sha(baseline_link) != profile["binary"]:
        raise ValueError("Baseline relink does not reproduce the pinned executable")
    shutil.copyfile(header, out/header.name)
    (out / "modem_main.c").write_text(source)
    flags = ["gcc", "-m32", "-Wall", "-g", "-O", "-I.", "-DCONFIG_DEBUG_MODEM"]
    # Prove the current source/headers/compiler reproduce baseline machine code
    # before trusting a same-binary off/on experiment. DWARF paths may differ.
    pristine = out / "pristine-host-check"
    pristine.mkdir()
    shutil.copyfile(base/"modem_main.c", pristine/"modem_main.c")
    subprocess.run([*flags, "-I"+str(base), "-c", "modem_main.c", "-o", "modem_main.o"],
                   cwd=pristine, check=True)
    original_code = code_evidence(base/"modem_main.o")
    if code_evidence(pristine/"modem_main.o") != original_code:
        raise ValueError("Pristine source/headers/compiler do not reproduce baseline code and relocations")
    subprocess.run([*flags, "-c", "modem_main.c", "-o", "modem_main.o"], cwd=out, check=True)
    subprocess.run(["gcc", "-m32", "-pie", "-o", "slmodemd", "modem_main.o", *objects, "-lm"], cwd=out, check=True)
    elf32_i386(out / "slmodemd", "DYN")
    for file, digest in inputs.items():
        if sha(base/file) != digest: raise ValueError("Base changed during build: " + file)
        if file not in ("modem_main.c", "modem_main.o", "slmodemd") and sha(out/file) != digest:
            raise ValueError("Reused input changed: " + file)
    if sha(header) != header_sha or sha(out/header.name) != header_sha: raise ValueError("Header changed")
    dis = subprocess.check_output(["objdump", "-drwC", str(out/"modem_main.o")], text=True)
    for symbol in ("recv", "send", "ppoll", "sigprocmask"):
        if not re.search(r"R_386_\w+\s+" + symbol + r"\b", dis):
            raise ValueError("Missing linked socket transport path: " + symbol)
    report = {"profile": name, "flag": "DMODEM_SOCKET_IO=1", "default_enabled": False,
              "base": str(base), "output": str(out), "base_input_sha256": inputs,
              "header_sha256": header_sha, "source_sha256": sha(out/"modem_main.c"),
              "host_object_sha256": sha(out/"modem_main.o"), "binary_sha256": sha(out/"slmodemd"),
              "dsp_unchanged": True, "link_type": "ELF32-i386-PIE", "make_invoked": False,
              "pristine_host_code_matches": True, "original_host_code": original_code,
              "baseline_relink_matches": True, "baseline_relinked_sha256": sha(baseline_link),
              "scope": "socket bytes and delay bounds only; no DSP/conditioning/clock modification"}
    (out / "socket-io-build.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
