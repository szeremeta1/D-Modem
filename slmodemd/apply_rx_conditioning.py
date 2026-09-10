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


def patch_source(source):
    source = once(source, "#include <modem_debug.h>",
                  '#include <modem_debug.h>\n#include "dmodem_rx_condition.h"\n'
                  'static rxcond_state socket_rxcond;')
    source = once(source, '\tDBG("socket_start...\\n");',
                  '\tDBG("socket_start...\\n");\n'
                  '\tif (rxcond_init(&socket_rxcond, MODEM_RATE) < 0) return -EINVAL;')
    source = once(source, '\tDBG("socket_stop...\\n");',
                  '\tDBG("socket_stop...\\n");\n'
                  '\tif (socket_rxcond.active) fprintf(stderr, '
                  '"rxcond: samples=%llu tx_samples=%llu processed=%llu skipped=%llu '
                  'clipped=%llu delay_adjustments=%llu model_failures=%llu\\n", '
                  '(unsigned long long)socket_rxcond.rx_samples, '
                  '(unsigned long long)socket_rxcond.tx_samples, '
                  '(unsigned long long)socket_rxcond.processed_samples, '
                  '(unsigned long long)socket_rxcond.skipped_samples, '
                  '(unsigned long long)socket_rxcond.clipped, '
                  '(unsigned long long)socket_rxcond.delay_adjustments, '
                  '(unsigned long long)socket_rxcond.model_failures);\n'
                  '\tsocket_rxcond.active = 0;')
    source = once(source, '\t\tret = write(dev->fd, outbuf, ret);',
                  '\t\tif (socket_rxcond.active && socket_rxcond.mode == RXCOND_SOCKET) {\n'
                  '\t\t\tint n = rxcond_write(&socket_rxcond, dev->fd, outbuf, ret/2);\n'
                  '\t\t\tret = n < 0 ? n : n*2;\n'
                  '\t\t} else ret = write(dev->fd, outbuf, ret);')
    source = once(source, '\tint ret = read(dev->fd, buf, size*2);',
                  '\tint ret;\n'
                  '\tif (socket_rxcond.active) '
                  'return rxcond_read(&socket_rxcond, dev->fd, buf, size);\n'
                  '\tret = read(dev->fd, buf, size*2);')
    source = once(source, '\tint ret = write(dev->fd, buf, size*2);',
                  '\tint ret;\n'
                  '\tif (socket_rxcond.active) '
                  'return rxcond_write(&socket_rxcond, dev->fd, buf, size);\n'
                  '\tret = write(dev->fd, buf, size*2);')
    source = once(source, '\t\t\tin = inbuf;',
                  '\t\t\tin = inbuf;\n'
                  '\t\t\tif (socket_rxcond.active && socket_rxcond.mode == RXCOND_SOCKET && m->update_delay < 0) {\n'
                  '\t\t\t\t++socket_rxcond.delay_adjustments;\n'
                  '\t\t\t\tfprintf(stderr, "rxcond: raw socket RX skip request %d '
                  'after received sample %llu\\n", m->update_delay, '
                  '(unsigned long long)socket_rxcond.rx_samples);\n'
                  '\t\t\t\tin = rxcond_skip_input(&socket_rxcond, (int16_t *)inbuf, '
                  '&count, &m->update_delay, &dev->delay);\n'
                  '\t\t\t\tif (count == 0) continue;\n'
                  '\t\t\t}')
    for sign in ('<', '>'):
        anchor = '\t\t\tif(m->update_delay ' + sign + ' 0) {'
        source = once(source, anchor, anchor + '\n'
                      '\t\t\t\tif (socket_rxcond.active) {\n'
                      '\t\t\t\t\t++socket_rxcond.delay_adjustments;\n'
                      '\t\t\t\t\tfprintf(stderr, "rxcond: host delay adjustment %d '
                      'at %s sample %llu\\n", m->update_delay, '
                      'socket_rxcond.mode == RXCOND_SOCKET ? "received socket" : "processed", '
                      '(unsigned long long)socket_rxcond.rx_samples);\n'
                      '\t\t\t\t}')
    source = once(source, '\t\t\tmodem_process(m,inbuf,outbuf,count);',
                  '\t\t\tif (socket_rxcond.active && socket_rxcond.mode == RXCOND_PROCESSED && '
                  'rxcond_rx(&socket_rxcond, (int16_t *)inbuf, count) < 0) '
                  '{ ERR("rxcond input alignment failed\\n"); return -1; }\n'
                  '\t\t\tif (socket_rxcond.active) socket_rxcond.processed_samples += count;\n'
                  '\t\t\tmodem_process(m, socket_rxcond.active && socket_rxcond.mode == RXCOND_SOCKET '
                  '? in : inbuf, outbuf, count);\n'
                  '\t\t\tif (socket_rxcond.active && socket_rxcond.mode == RXCOND_PROCESSED && '
                  'rxcond_tx(&socket_rxcond, (const int16_t *)outbuf, count) < 0) '
                  '{ ERR("rxcond output alignment failed\\n"); return -1; }')
    source = once(source, '\t\t\t\tmemset(outbuf, 0, m->update_delay*2);',
                  '\t\t\t\tif (socket_rxcond.active && socket_rxcond.mode == RXCOND_SOCKET && '
                  '(unsigned)m->update_delay > sizeof(outbuf)/2) '
                  '{ ERR("rxcond padding exceeds output buffer\\n"); return -1; }\n'
                  '\t\t\t\tmemset(outbuf, 0, m->update_delay*2);')
    return source


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
    source = patch_source((base / "modem_main.c").read_text())
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
                  "conditioning_api": "processed-pilot-or-causal-socket-index-v3",
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
