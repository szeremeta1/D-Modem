#!/usr/bin/env python3
"""Linux build/install checks in disposable directories; never start a modem."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1]


def digest_tree(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob("*") if p.is_file()}


def run():
    with tempfile.TemporaryDirectory(prefix="dmodem watchdog install ") as tmp:
        root = Path(tmp)
        src = root / "source"
        shutil.copytree(SOURCE, src)
        pristine = digest_tree(src)
        base_env = {k: v for k, v in os.environ.items()
                    if k not in {"MAKE", "NM", "OBJCOPY", "OBJDUMP"}}

        def invoke(output, *, env=None, ok=False):
            # Both the script and source arguments are relative; the output and
            # parent paths contain spaces. This reproduces the old cd/$0 bug.
            result = subprocess.run(["./source/apply_watchdog_hook.sh", "./source", str(output)],
                                    cwd=root, env=base_env | (env or {}),
                                    capture_output=True, text=True)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            assert digest_tree(src) == pristine, "source tree changed"
            assert not list(root.glob(".slm-v34-watchdog.*")), "abandoned stage"
            return result

        output = root / "verified output"
        invoke(output, ok=True)
        installed = digest_tree(output)
        assert "verified existing build" in invoke(output, ok=True).stdout
        assert digest_tree(output) == installed
        print("PASS relative invocation, spaces, pristine source, verified idempotency")

        binary = output / "slmodemd"
        binary.write_bytes(binary.read_bytes() + b"changed")
        changed = digest_tree(output)
        assert "existing output changed" in invoke(output).stderr
        assert digest_tree(output) == changed
        print("PASS tampered output refused and preserved")

        unknown = root / "unrelated"
        unknown.mkdir()
        (unknown / "keep").write_text("unrelated output")
        before = digest_tree(unknown)
        invoke(unknown)
        assert digest_tree(unknown) == before
        print("PASS unrelated output refused and preserved")

        failed = root / "failed build"
        invoke(failed, env={"MAKE": shutil.which("false")})
        assert not failed.exists()
        print("PASS failed build leaves no output and no source mutation")

        invoke(src / "nested output")
        assert not (src / "nested output").exists()
        print("PASS output inside source refused")

        # The vendor object must be checked before staging or mutation.
        blob = src / "dsplibs.o"
        original = blob.read_bytes()
        blob.write_bytes(original + b"unsupported")
        pristine = digest_tree(src)
        rejected = root / "unsupported"
        assert "unsupported dsplibs.o SHA-256" in invoke(rejected).stderr
        assert not rejected.exists()
        blob.write_bytes(original)
        pristine = digest_tree(src)
        print("PASS unsupported ABI refused before staging")

        # A syntactically linked binary with the wrong vendor caller must fail
        # the actual call-site assertion, not merely pass a symbol-name test.
        make = shlex.quote(shutil.which("make"))
        objcopy = shlex.quote(shutil.which("objcopy"))
        shim = root / "make wrong caller"
        shim.write_text(f'#!/bin/sh\nset -eu\n{make} "$@"\n{objcopy} --redefine-sym vpcm_run=wrong_vendor_caller slmodemd\n')
        shim.chmod(0o700)
        missing = root / "wrong caller output"
        assert "vpcm_run does not call" in invoke(missing, env={"MAKE": str(shim)}).stderr
        assert not missing.exists()
        print("PASS wrong caller rejected despite wrapper/original symbols")

        race = root / "concurrent output"
        shim.write_text(f'#!/bin/sh\nset -eu\n{make} "$@"\nmkdir -- "$WATCHDOG_RACE_OUTPUT"\n')
        race_result = invoke(race, env={"MAKE": str(shim), "WATCHDOG_RACE_OUTPUT": str(race)})
        assert "output appeared during build" in race_result.stderr, race_result.stdout + race_result.stderr
        assert race.is_dir() and not list(race.iterdir())
        print("PASS concurrently created output preserved")


if __name__ == "__main__":
    run()
