import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('smoke_native', ROOT / 'tools/smoke_native.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

class FixtureContractTest(unittest.TestCase):
    def test_fixture_header_and_length(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'sample.bin'
            mod.fixture(path)
            data = path.read_bytes()
            self.assertEqual(data[:8], b'DGNUM006')
            self.assertEqual(struct.unpack_from('<4I', data, 8), (64, 1, 32, 2))
            self.assertEqual(struct.unpack_from('<2I', data, 24), (63, 64))
            self.assertEqual(len(data), 8 + 16 + 8 + 2 * ((64 + 64) * 32 + 2 * 32))
    def test_manifest_format(self):
        jobs = json.loads((ROOT / 'output/ecp/ALL_REGISTERED_TASKS.json').read_text())
        self.assertGreater(len(jobs), 0)
        self.assertEqual(len({x['name'] for x in jobs}), len(jobs))
        self.assertTrue(all('input' in x for x in jobs))
    def test_all_vendored_licenses(self):
        base = ROOT / 'native_base/baseline/reproduction/native/native_source'
        for name in ('argparse', 'spdlog', 'yaml-cpp'):
            self.assertTrue((base / 'ext' / name / 'LICENSE').is_file())

if __name__ == '__main__':
    unittest.main()
