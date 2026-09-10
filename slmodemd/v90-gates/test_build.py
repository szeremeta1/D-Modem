#!/usr/bin/env python3
"""Linux build/ABI/rollback checks in scratch directories. Never place a call."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1]


def hashes(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


def main():
    with tempfile.TemporaryDirectory(prefix="dmodem v90 build ") as tmp:
        root = Path(tmp)
        src = root / "source"
        shutil.copytree(SOURCE, src)
        pristine = hashes(src)
        env = dict(os.environ)

        def invoke(output, extra=None, ok=False):
            result = subprocess.run(["python3", "./source/v90-gates/build.py", "./source", str(output)],
                                    cwd=root, env=env | (extra or {}), text=True, capture_output=True)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            assert hashes(src) == pristine, "input source was changed"
            assert not list(root.glob(".dmodem-v90-gates-*")), "temporary build leaked"
            return result

        out = root / "compiled output"
        invoke(out, ok=True)
        manifest = json.loads((out / "gate-build.json").read_text())
        assert manifest["source_sha256"] == hashlib.sha256((src / "dsplibs.o").read_bytes()).hexdigest()
        assert manifest["binary_sha256"] == hashlib.sha256((out / "slmodemd").read_bytes()).hexdigest()
        assert manifest["wrapper_sha256"] == hashlib.sha256((src / "v90-gates/v90_gate.c").read_bytes()).hexdigest()
        assert manifest["host_transition_verified"] is True
        assert manifest["source_host_sha256"] != manifest["conditioned_host_sha256"]
        elf = subprocess.check_output(["readelf", "-h", str(out / "slmodemd")], text=True)
        assert "ELF32" in elf and "Intel 80386" in elf, elf
        check = subprocess.run(["timeout", "15", str(out / "offline-check")], text=True, capture_output=True)
        assert check.returncode == 0, check.stdout + check.stderr
        assert "OFFLINE GATE CHECKS PASS" in check.stdout
        for evidence in ("rejected-PCM-does-not-arm-constructor", "actual-V34-fallback", "selected-digital",
                         "new-V8-before-selection", "selection-rejected-on-later-update", "invalid-JM-bit-count",
                         "host-transition-to-V34-clears-selection", "unsupported-V92-answer",
                         "enabled and disabled controls identical"):
            assert evidence in check.stdout, evidence
        print("PASS stock i386 link, wrapper/original and host gate targets, constructor/menu/control regressions")

        installed = hashes(out)
        invoke(out)
        assert hashes(out) == installed
        print("PASS existing completed output preserved")
        unknown = root / "unrelated output"
        unknown.mkdir()
        (unknown / "keep").write_text("do not overwrite")
        prior = hashes(unknown)
        invoke(unknown)
        assert hashes(unknown) == prior
        print("PASS unrelated output preserved")
        invoke(src / "nested")
        link = root / "output link"
        link.symlink_to(root / "missing", target_is_directory=True)
        invoke(link)
        assert link.is_symlink() and not link.exists()
        print("PASS nested output and dangling symlink refused")

        tools = root / "tools"
        tools.mkdir()
        make = tools / "make"
        make.write_text("#!/bin/sh\nexit 23\n")
        make.chmod(0o700)
        path = str(tools) + os.pathsep + env["PATH"]
        failure = root / "failed output"
        invoke(failure, {"PATH": path})
        assert not failure.exists()
        print("PASS failed compilation leaves no partial output")

        actual_make = shlex.quote(shutil.which("make"))
        make.write_text(f'#!/bin/sh\nset -eu\n{actual_make} "$@"\nmkdir -- "$DMODEM_TEST_OUTPUT"\n')
        race = root / "concurrent output"
        invoke(race, {"PATH": path, "DMODEM_TEST_OUTPUT": str(race)})
        assert race.is_dir() and not list(race.iterdir())
        print("PASS concurrent output preserved")

        host = src / "modem.c"
        original_host = host.read_bytes()
        host.write_text(host.read_text().replace("op = get_dp_operations(id);", "/* incompatible host */"))
        pristine = hashes(src)
        rejected = root / "bad host output"
        assert "host datapump transition anchor mismatch" in invoke(rejected).stderr
        assert not rejected.exists()
        host.write_bytes(original_host)
        print("PASS incompatible host refused before staging")

        obj = src / "dsplibs.o"
        data = bytearray(obj.read_bytes()); data[-1] ^= 1; obj.write_bytes(data)
        pristine = hashes(src)
        rejected = root / "unknown ABI output"
        assert "unknown DSP object" in invoke(rejected).stderr
        assert not rejected.exists()
        print("PASS unknown object refused before any opaque ABI access")


if __name__ == "__main__":
    main()
