#!/usr/bin/env python3
"""Refusal and source-scope tests. Writes only new temporary test directories."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("socket_builder", HERE/"build.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def refused(base, out, expected):
    p = subprocess.run([sys.executable, str(HERE/"build.py"), str(base), str(out)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
    assert p.returncode and expected in p.stdout, p.stdout


def copy_minimum(base, out):
    out.mkdir()
    for name in ("modem_main.c", "modem_main.o", "dsplibs.o", "slmodemd"):
        shutil.copyfile(base/name,out/name)


def main():
    base = Path(sys.argv[1]).resolve()
    original = (base/"modem_main.c").read_text()
    patched = builder.patch_source(original)
    # The entire ALSA implementation and old hardware-driver implementation
    # remain literal-identical. Only socket_start binds the enabled device.
    for begin,end in [("#ifdef SUPPORT_ALSA\n\n#define INTERNAL_DELAY", "/*\n *    'driver' stuff"),
                      ("static int modemap_start", "static int socket_start")]:
        assert original[original.index(begin):original.index(end)] == patched[patched.index(begin):patched.index(end)]
    assert "#define SOCKET_IO_ACTIVE(d) (socket_io.enabled && socket_io_device == (d))" in patched
    assert "modem_process(m, SOCKET_IO_ACTIVE(dev) ? in : inbuf, outbuf, count)" in patched
    assert patched.count("sio_padding(&socket_io,") == 2
    assert patched.index("invalid output delay before block write") < patched.index("count = device_write(dev,outbuf,count)")
    assert "sample_rate / 50" not in patched and "rxcond_" not in patched
    try:
        builder.patch_source(patched)
    except ValueError:
        pass
    else:
        raise AssertionError("double patch was accepted")
    before = {p.name:builder.sha(p) for p in base.iterdir() if p.is_file() and not p.is_symlink()}
    with tempfile.TemporaryDirectory(prefix="dmodem-socket-builder-test-") as temp:
        root = Path(temp)
        for field,expected in [("modem_main.c","Unreviewed host source hash"),
                               ("modem_main.o","Unreviewed baseline modem_main.o"),
                               ("dsplibs.o","Unreviewed baseline dsplibs.o"),
                               ("slmodemd","Unreviewed baseline slmodemd")]:
            trial=root/field.replace(".","-")
            copy_minimum(base,trial)
            with (trial/field).open("ab") as f:f.write(b"X")
            out=root/(trial.name+"-output")
            refused(trial,out,expected);assert not out.exists()
        refused(base,base,"Output must be new")
        existing=root/"existing";existing.mkdir()
        refused(base,existing,"Output must be new")
        linked=root/"linked";copy_minimum(base,linked)
        (linked/"modem_main.c").unlink();(linked/"modem_main.c").symlink_to(base/"modem_main.c")
        refused(linked,root/"linked-output","Refusing linked input modem_main.c")
        # A layout-changing header with unchanged source/blob/object pins must
        # fail the pristine code/relocation comparison, before final linking.
        drift=root/"header-drift";drift.mkdir()
        profile=next(p for p in builder.PROFILES.values() if p["source"]==builder.sha(base/"modem_main.c"))
        names=["modem_main.c","modem_main.o","slmodemd"]+[n+".o" for n in profile["objects"]]
        names += [p.name for p in base.glob("*.h") if not p.is_symlink()]
        for name in names:shutil.copyfile(base/name,drift/name)
        common_drift=root/"common-object-drift";shutil.copytree(drift,common_drift)
        subprocess.run(["objcopy","--strip-debug",str(common_drift/"modem_timer.o")],check=True)
        assert builder.sha(common_drift/"modem_timer.o")!=builder.sha(base/"modem_timer.o")
        refused(common_drift,root/"common-object-output","Baseline relink does not reproduce the pinned executable")
        header=drift/"modem.h";text=header.read_text()
        assert text.count("dial_string[128]")==1
        header.write_text(text.replace("dial_string[128]","dial_string[132]"))
        refused(drift,root/"header-output","do not reproduce baseline code and relocations")
    after = {p.name:builder.sha(p) for p in base.iterdir() if p.is_file() and not p.is_symlink()}
    assert before==after
    print("BUILDER CHECKS PASS: four changed pins, common-object drift, existing/base output, symlink input, duplicate patch and changed header layout refused; ALSA/hardware source and all base files unchanged")


if __name__ == "__main__": main()
