#!/usr/bin/env python3
"""Input refusal, isolated staging and offline evidence regression fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


b = load('build')
v = load('validate')


class Inputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        abi = json.loads((HERE / 'PUBLIC-ABI.json').read_text())
        for name in [*abi['public_source_sha256'], 'dsplibs.o']:
            shutil.copyfile(HERE.parent / name, self.source / name)
        (self.source / 'v90-gates').mkdir()
        for name in abi['public_gate_source_sha256']:
            shutil.copyfile(HERE.parent / 'v90-gates' / name, self.source / 'v90-gates' / name)
        self.output = self.root / 'new-output'

    def test_exact_public_inventory(self):
        abi, inputs = b.inventory(self.source, self.output)
        self.assertEqual(len(inputs), 1 + len(abi['public_source_sha256']) +
                         len(abi['public_gate_source_sha256']) + len(abi['probe_source_sha256']))
        self.assertFalse(self.output.exists())

    def test_changed_header_refused(self):
        (self.source / 'modem.h').write_text('changed ABI')
        with self.assertRaises(ValueError): b.inventory(self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_changed_gate_refused(self):
        (self.source / 'v90-gates/build.py').write_text('unreviewed builder')
        with self.assertRaises(ValueError): b.inventory(self.source, self.output)

    def test_changed_dsp_refused(self):
        (self.source / 'dsplibs.o').write_bytes(b'other vendor object')
        with self.assertRaises(ValueError): b.inventory(self.source, self.output)

    def test_symlink_input_refused(self):
        p = self.source / 'modem.h'
        p.unlink(); p.symlink_to(HERE.parent / 'modem.h')
        with self.assertRaises(ValueError): b.inventory(self.source, self.output)

    def test_existing_and_overlapping_outputs_refused(self):
        self.output.mkdir()
        for path in (self.output, self.source / 'output', self.root):
            with self.subTest(path=path), self.assertRaises(ValueError):
                b.inventory(self.source, path)

    def test_extra_objects_and_make_includes_are_not_staged(self):
        for name in ('.depend', '.build_profile', 'modem.o', 'slmodemd'):
            (self.source / name).write_text('untrusted prior build content')
        _, inputs = b.inventory(self.source, self.output)
        b.stage_public(inputs, self.source, self.output)
        for name in ('.depend', '.build_profile', 'modem.o', 'slmodemd'):
            self.assertFalse((self.output / name).exists())
        self.assertEqual(b.sha(self.output / 'dsplibs.o'), b.sha(self.source / 'dsplibs.o'))

    def test_build_environment_is_not_inherited(self):
        old = os.environ.get('MAKEFLAGS')
        os.environ['MAKEFLAGS'] = 'UNREVIEWED=1'
        try:
            b.run_owned([sys.executable, '-c',
                         "import os; assert 'MAKEFLAGS' not in os.environ"],
                        self.root / 'environment.log')
        finally:
            if old is None: os.environ.pop('MAKEFLAGS', None)
            else: os.environ['MAKEFLAGS'] = old

    def test_failed_command_is_not_success(self):
        with self.assertRaises(ValueError):
            b.run_owned([sys.executable, '-c', 'raise SystemExit(3)'], self.root / 'failure.log')

    def test_timeout_is_not_success(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            b.run_owned([sys.executable, '-c', 'import time; time.sleep(30)'],
                        self.root / 'timeout.log', timeout=0.1)


class Evidence(unittest.TestCase):
    def test_missing_or_ambiguous_trace_refused(self):
        for raw in ('', 'openat(unfinished', '+++ exited with 0 +++',
                    'open("relative", O_RDONLY) = 3',
                    'socket(AF_INET, SOCK_STREAM, 0) = 3',
                    'open("/tmp/../dev/tty", O_RDONLY) = 3'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                v.trace_summary(raw)

    def test_real_library_open_is_bounded_evidence(self):
        result = v.trace_summary('openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY) = 3')
        self.assertEqual(result['parsed_records'], 1)

    def test_empty_probe_output_refused(self):
        for name in b.PROBES:
            with self.subTest(name=name), self.assertRaises(ValueError):
                v.result_summary(name, '')


if __name__ == '__main__':
    unittest.main()
