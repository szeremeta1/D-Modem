#!/usr/bin/env python3
"""Build optional signed-linear RX conditioning in a separate D-Modem tree.

Linux: python3 slmodemd/apply_rx_conditioning.py SOURCE [OUTPUT]
Uses the public source Makefile and leaves SOURCE and its DSP object unchanged.
No modem or SIP call is started. Controls default off.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Source anchor must occur once: {old[:65]!r}")
    return text.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="stock slmodemd source directory")
    parser.add_argument("output", type=Path, nargs="?", help="new directory; defaults to SOURCE-rxcond")
    args = parser.parse_args()
    base = args.source.resolve()
    requested = args.output if args.output is not None else Path(str(base) + "-rxcond")
    if requested.is_symlink():
        parser.error("output must not be a symlink")
    out = requested.resolve()
    header = Path(__file__).resolve().with_name("dmodem_rx_condition.h")
    if out.exists() or base in out.parents or out == base:
        parser.error("output must be a new directory outside the source tree")
    if not out.parent.is_dir():
        parser.error("output parent directory must already exist")
    for filename in ("modem_main.c", "Makefile", "dsplibs.o"):
        if not (base / filename).is_file():
            parser.error(f"source is missing {filename}")
    source = (base / "modem_main.c").read_text()
    source = once(source, "#include <modem_debug.h>",
                  '#include <modem_debug.h>\n#include "dmodem_rx_condition.h"\n'
                  'static rxcond_state socket_rxcond;')
    source = once(source, '\tDBG("socket_start...\\n");',
                  '\tDBG("socket_start...\\n");\n'
                  '\tif (rxcond_init(&socket_rxcond, MODEM_RATE) < 0) return -EINVAL;')
    source = once(source, '\tDBG("socket_stop...\\n");',
                  '\tDBG("socket_stop...\\n");\n'
                  '\tif (socket_rxcond.active) fprintf(stderr, '
                  '"rxcond: samples=%llu clipped=%llu delay_adjustments=%llu\\n", '
                  '(unsigned long long)socket_rxcond.rx_samples, '
                  '(unsigned long long)socket_rxcond.clipped, '
                  '(unsigned long long)socket_rxcond.delay_adjustments);\n'
                  '\tsocket_rxcond.active = 0;')
    source = once(source, '\tint ret = read(dev->fd, buf, size*2);',
                  '\tint ret;\n'
                  '\tif (socket_rxcond.active) '
                  'return rxcond_read(&socket_rxcond, dev->fd, buf, size);\n'
                  '\tret = read(dev->fd, buf, size*2);')
    source = once(source, '\tint ret = write(dev->fd, buf, size*2);',
                  '\tint ret = write(dev->fd, buf, size*2);\n'
                  '\tif (socket_rxcond.active && ret >= 0 && ret != size*2) '
                  '{ ERR("rxcond incomplete socket write\\n"); errno = EIO; return -1; }')
    for sign in ('<', '>'):
        anchor = '\t\t\tif(m->update_delay ' + sign + ' 0) {'
        source = once(source, anchor, anchor + '\n'
                      '\t\t\t\tif (socket_rxcond.active) {\n'
                      '\t\t\t\t\t++socket_rxcond.delay_adjustments;\n'
                      '\t\t\t\t\tfprintf(stderr, "rxcond: host delay adjustment %d '
                      'at processed sample %llu\\n", m->update_delay, '
                      '(unsigned long long)socket_rxcond.rx_samples);\n'
                      '\t\t\t\t}')
    source = once(source, '\t\t\tmodem_process(m,inbuf,outbuf,count);',
                  '\t\t\tif (rxcond_rx(&socket_rxcond, (int16_t *)inbuf, count) < 0) '
                  '{ ERR("rxcond input alignment failed\\n"); return -1; }\n'
                  '\t\t\tmodem_process(m,inbuf,outbuf,count);\n'
                  '\t\t\tif (socket_rxcond.active && '
                  'rxcond_tx(&socket_rxcond, (const int16_t *)outbuf, count) < 0) '
                  '{ ERR("rxcond output alignment failed\\n"); return -1; }')
    # This hook uses a public host-source boundary, with no private DSP offsets.
    # Require unique anchors, preserve the vendor object exactly, and let this
    # source tree's own Makefile select its objects and 32-bit toolchain.
    makefile = (base / "Makefile").read_text() + "\n# Optional host RX conditioning uses libm.\nLFLAGS += -lm\n"
    stage = Path(tempfile.mkdtemp(prefix=".dmodem-rxcond-", dir=out.parent))
    try:
        shutil.copytree(base, stage, dirs_exist_ok=True)
        (stage / "modem_main.c").write_text(source)
        (stage / "Makefile").write_text(makefile)
        shutil.copyfile(header, stage / "dmodem_rx_condition.h")
        for obj in stage.glob("*.o"):
            if obj.name != "dsplibs.o":
                obj.unlink()
        for filename in ("slmodemd", "modem_test", ".build_profile", ".depend"):
            (stage / filename).unlink(missing_ok=True)
        subprocess.run(["make", "-s", "slmodemd"], cwd=stage, check=True)
        if sha(base / "dsplibs.o") != sha(stage / "dsplibs.o"):
            raise RuntimeError("vendor object changed during build")
        symbols = subprocess.check_output(["nm", "slmodemd"], cwd=stage, text=True)
        if not any(line.split()[-1:] == ["socket_rxcond"] for line in symbols.splitlines()):
            raise RuntimeError("linked executable has no conditioning state")
        result = {"source": str(base), "output": str(out),
                  "source_sha256": sha(base / "modem_main.c"),
                  "conditioned_source_sha256": sha(stage / "modem_main.c"),
                  "header_sha256": sha(header),
                  "binary_sha256": sha(stage / "slmodemd"),
                  "dsp_sha256": sha(stage / "dsplibs.o"),
                  "dsp_unchanged": True}
        (stage / "rx-conditioning-build.json").write_text(json.dumps(result, indent=2) + "\n")
        # GNU mv's no-clobber return behavior differs between releases. Check
        # both the status and source existence, preserving a concurrent output.
        moved = subprocess.run(["mv", "-T", "-n", "--", str(stage), str(out)])
        if moved.returncode or stage.exists():
            raise RuntimeError("output appeared during build or publication failed")
        print(json.dumps(result, indent=2))
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == "__main__":
    main()
