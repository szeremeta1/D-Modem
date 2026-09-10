#!/usr/bin/env python3
"""Linux component build checks in disposable trees; never start a modem."""
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
    with tempfile.TemporaryDirectory(prefix="dmodem rx build ") as tmp:
        root = Path(tmp)
        src = root / "source"
        shutil.copytree(SOURCE, src)
        pristine = hashes(src)
        env = dict(os.environ)

        def invoke(output, extra=None, ok=False):
            result = subprocess.run(["python3", "./source/apply_rx_conditioning.py", "./source", str(output)],
                                    cwd=root, env=env | (extra or {}), text=True, capture_output=True)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            assert hashes(src) == pristine, "input source was changed"
            assert not list(root.glob(".dmodem-rxcond-*")), "temporary build leaked"
            return result

        out = root / "compiled output"
        invoke(out, ok=True)
        manifest = json.loads((out / "rx-conditioning-build.json").read_text())
        assert manifest["dsp_unchanged"] is True
        assert manifest["dsp_sha256"] == hashlib.sha256((src / "dsplibs.o").read_bytes()).hexdigest()
        assert manifest["header_sha256"] == hashlib.sha256((src / "dmodem_rx_condition.h").read_bytes()).hexdigest()
        assert manifest["binary_sha256"] == hashlib.sha256((out / "slmodemd").read_bytes()).hexdigest()
        elf = subprocess.check_output(["readelf", "-h", str(out / "slmodemd")], text=True)
        assert "ELF32" in elf and "Intel 80386" in elf, elf
        print("PASS stock i386 build, public Makefile, exact header/blob, source preservation")

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
        print("PASS compilation failure leaves no partial output")

        actual_make = shlex.quote(shutil.which("make"))
        make.write_text(f'#!/bin/sh\nset -eu\n{actual_make} "$@"\nmkdir -- "$DMODEM_TEST_OUTPUT"\n')
        race = root / "concurrent output"
        invoke(race, {"PATH": path, "DMODEM_TEST_OUTPUT": str(race)})
        assert race.is_dir() and not list(race.iterdir())
        print("PASS concurrent output preserved")

        # Detect incompatible host source before copying or compiling anything.
        main_source = src / "modem_main.c"
        main_source.write_text(main_source.read_text().replace("modem_process(m,inbuf,outbuf,count);", "/* incompatible source */"))
        pristine = hashes(src)
        rejected = root / "bad source output"
        assert "Source anchor must occur once" in invoke(rejected).stderr
        assert not rejected.exists()
        print("PASS incompatible source refused before staging")


if __name__ == "__main__":
    main()
