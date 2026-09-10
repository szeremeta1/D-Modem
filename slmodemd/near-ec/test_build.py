#!/usr/bin/env python3
"""Check build refusal and call-binding contracts against a prebuilt baseline."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("near_builder", HERE / "build.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def hashes(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


def main():
    baseline = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="near ec build ") as temp:
        root = Path(temp)
        source = root / "baseline"
        shutil.copytree(baseline, source)
        pristine = hashes(source)
        env = dict(os.environ)
        tools = root / "tools"
        tools.mkdir()
        make = tools / "make"
        make.write_text("#!/bin/sh\nexit 93\n")
        make.chmod(0o700)
        env["PATH"] = str(tools) + os.pathsep + env["PATH"]

        def invoke(out, extra=None, ok=False):
            result = subprocess.run(["python3", str(HERE / "build.py"), "./baseline", str(out)],
                                    cwd=root, env=env | (extra or {}), text=True, capture_output=True)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            assert hashes(source) == pristine, "baseline changed"
            assert not list(root.glob(".dmodem-near-ec-*")), "staging tree leaked"
            return result

        out = root / "compiled output"
        invoke(out, ok=True)
        report = json.loads((out / "near-ec-build.json").read_text())
        assert report["wrapper_local_delegation_verified"] is True
        for name, digest in report["reused_input_sha256"].items():
            assert hashlib.sha256((source / name).read_bytes()).hexdigest() == digest
            if name not in ("slmodemd", "dsplibs.o"):
                assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest
        checked = subprocess.run(["python3", str(HERE / "check.py"), str(out)], text=True, capture_output=True)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert "PASS: 10 process-isolated" in checked.stdout
        print("PASS no-make build, exact prebuilt objects, both layouts and all real caller fixtures")

        syms = builder.nm(out / "slmodemd")
        dis = builder.run("objdump", "-d", out / "slmodemd")
        builder.near_bindings(syms, dis)

        def rejects(symbols, assembly):
            try:
                builder.near_bindings(symbols, assembly)
            except ValueError:
                return
            raise AssertionError("invalid call-binding evidence accepted")

        weak = dict(syms)
        weak["V34EchoFilter"] = (weak["V34EchoFilter"][0], "W")
        rejects(weak, dis)
        for marker in builder.RETURNS:
            at = syms[marker][0] - 5
            changed = re.sub(rf"(^\s*{at:x}:.*)<V34EchoFilter>", r"\1<wrong_target>", dis, flags=re.M)
            assert changed != dis
            rejects(syms, changed)
        # Leave a valid-looking original call elsewhere, but remove the call
        # within the wrapper. Whole-executable text search would accept this.
        body = re.search(r"^[0-9a-fA-F]+ <V34EchoFilter>:\n(.*?)(?=^[0-9a-fA-F]+ <|\Z)", dis, re.M | re.S)
        broken = body[0].replace("<V34EchoFilter__near_original>", "<wrong_target>")
        changed = dis.replace(body[0], broken)
        changed += "\n00000001 <unrelated_probe>:\n 1: e8 00 00 00 00 call " + format(syms["V34EchoFilter__near_original"][0], "x") + " <V34EchoFilter__near_original>\n"
        rejects(syms, changed)
        print("PASS weak wrapper, each misbound marker, and unrelated-original-call evidence refused")

        installed = hashes(out)
        invoke(out)
        assert hashes(out) == installed
        nested = source / "nested"
        invoke(nested)
        link = root / "dangling output"
        link.symlink_to(root / "missing", target_is_directory=True)
        invoke(link)
        assert link.is_symlink() and not link.exists()
        print("PASS existing/nested/symlink outputs preserved")

        gcc = tools / "gcc"
        gcc.write_text("#!/bin/sh\nexit 29\n")
        gcc.chmod(0o700)
        failure = root / "failed output"
        invoke(failure)
        assert not failure.exists()
        print("PASS compiler failure leaves no partial output")
        actual_gcc = shlex.quote(shutil.which("gcc"))
        gcc.write_text(f'#!/bin/sh\nset -eu\n{actual_gcc} "$@"\nmkdir -p -- "$DMODEM_TEST_OUTPUT"\nprintf preserved > "$DMODEM_TEST_OUTPUT/keep"\n')
        race = root / "concurrent output"
        invoke(race, {"DMODEM_TEST_OUTPUT": str(race)})
        assert (race / "keep").read_text() == "preserved" and len(list(race.iterdir())) == 1
        gcc.unlink()
        print("PASS concurrent output preserved")

        host = source / "modem_debug.o"
        original_host = host.read_bytes()
        subprocess.run(["objcopy", "--add-symbol=V34EchoFilter=.text:0,global,function", str(host)], check=True)
        pristine = hashes(source)
        refused = root / "competing wrapper"
        assert "competing echo wrapper" in invoke(refused).stderr
        assert not refused.exists()
        host.write_bytes(original_host)
        obj = source / "dsplibs.o"
        original_obj = obj.read_bytes()
        changed = bytearray(original_obj); changed[-1] ^= 1; obj.write_bytes(changed)
        pristine = hashes(source)
        refused = root / "unknown DSP"
        assert "unknown DSP" in invoke(refused).stderr
        assert not refused.exists()
        obj.write_bytes(original_obj)
        host.unlink()
        pristine = hashes(source)
        refused = root / "missing prebuilt object"
        assert "prebuilt regular file" in invoke(refused).stderr
        assert not refused.exists()
        print("PASS competing wrapper, unknown DSP and missing baseline object refused")


if __name__ == "__main__":
    main()
