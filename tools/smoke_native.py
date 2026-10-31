#!/usr/bin/env python3
"""Run a small deterministic fixture against a locally compiled simulator."""
import argparse
import hashlib
import os
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'native_base/baseline/reproduction/native/BASE_CONFIG.yaml'
ARRAYS = ('output.f32', 'scores.f32', 'probability.f16',
          'mask.u8', 'encoded_keys.f16', 'anchor.f32')

def fixture(path: Path, n=64, h=1, d=32, q=2):
    if n < q or d % 16 or h < 1 or q < 1:
        raise ValueError("Invalid fixture shape")
    lengths = tuple(range(n - q + 1, n + 1))
    # The fixture uses small finite half-precision values that are exact in FP32.
    key = [((i * 7 + j * 3) % 17 - 8) / 32 for i in range(n) for j in range(d)]
    value = [((i * 5 + j * 11) % 23 - 11) / 32 for i in range(n) for j in range(d)]
    query = [((t * 13 + j * 7) % 19 - 9) / 32 for t in range(q * h) for j in range(d)]
    with path.open('wb') as out:
        out.write(b'DGNUM006')
        out.write(struct.pack('<4I', n, h, d, q))
        out.write(struct.pack('<' + str(q) + 'I', *lengths))
        for values in (key, value, query):
            out.write(struct.pack('<' + str(len(values)) + 'e', *values))

def config(path: Path, inp: Path, dest: Path, trace: Path, raw: Path, ecp: bool):
    # Append settings at the MemorySystem mapping level and keep the
    # shipping HBM3 timing and command-model configuration unchanged.
    text = BASE.read_text()
    text = text.replace('        impl: HBM3TraceRecorder',
                        '        impl: HBM3TraceRecorder\n        path: ' + str(trace))
    text += (f'  fixture: {inp}\n  output_directory: {dest}\n'
             '  mode: dense\n  cache_initialization: existing\n'
             '  watchdog: 5000000\n'
             f'  encoder_enabled: true\n  encoder_raw_keys: {raw}\n'
             '  stream_max_fusion: true\n'
             f'  inplace_exp: {"false" if ecp else "true"}\n')
    if ecp:
        text += ('  qk_reuse_pipeline: true\n  qk_priority_first: true\n'
                 '  qk_readout_first: true\n  qk_exact_exp_cache: true\n')
    path.write_text(text)

def run(binary: Path, inp: Path, raw: Path, work: Path, index: int, ecp: bool):
    out = work / f'run{index}'
    out.mkdir()
    path = work / f'config{index}.yaml'
    config(path, inp, out, out / 'commands.log', raw, ecp)
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(binary.parent) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    completed = subprocess.run([str(binary), '-f', str(path)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, timeout=90)
    if completed.returncode != 0:
        raise RuntimeError(f'Native execution failed: {completed.stderr[-3000:]}\n{completed.stdout[-1000:]}')
    if not (out / 'result.yaml').is_file():
        raise RuntimeError('Simulator did not write result.yaml')
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', type=Path, default=ROOT / 'build/ramulator2')
    args = ap.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit(f'Missing simulator binary: {binary}. Run scripts/build_simulator.sh first.')
    with tempfile.TemporaryDirectory(prefix='dgpim_smoke_') as tmp:
        work = Path(tmp)
        checked = 0
        for case, (n, h, d, q) in enumerate(((64, 1, 32, 2), (128, 3, 64, 4))):
            workspace = work / f'case{case}'
            workspace.mkdir()
            inp = workspace / 'input.bin'
            fixture(inp, n=n, h=h, d=d, q=q)
            raw = workspace / 'raw_keys.f32'
            header = 8 + 16 + q * 4
            fp16 = struct.unpack_from('<' + str(n * d) + 'e', inp.read_bytes(), header)
            raw.write_bytes(struct.pack('<' + str(n * d) + 'f', *fp16))
            serial = run(binary, inp, raw, workspace, 1, ecp=False)
            pipeline = run(binary, inp, raw, workspace, 2, ecp=True)
            for name in ARRAYS:
                left, right = serial / name, pipeline / name
                if not left.is_file() or not right.is_file():
                    raise RuntimeError(f'Missing native output: {name}')
                a, b = left.read_bytes(), right.read_bytes()
                if not a or hashlib.sha256(a).digest() != hashlib.sha256(b).digest():
                    raise RuntimeError(f'Serial and ECP differ: {name}, case {case}')
                checked += 1
        # A corrupt fixture must never be reported as a completed native run.
        bad = work / 'corrupt.bin'
        bad.write_bytes(b'INVALID!' + b'\x00' * 24)
        bad_dir = work / 'rejected'
        bad_dir.mkdir()
        bad_conf = work / 'rejected.yaml'
        config(bad_conf, bad, bad_dir, bad_dir / 'commands.log', raw, False)
        env = dict(os.environ)
        env['LD_LIBRARY_PATH'] = str(binary.parent) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
        rejected = subprocess.run([str(binary), '-f', str(bad_conf)],
                                  env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=30)
        if rejected.returncode == 0 or (bad_dir / 'result.yaml').exists():
            raise RuntimeError('Corrupt fixture was accepted')
        print(f'Native smoke passed: {checked} bitwise serial/ECP array checks and corrupt-input rejection')

if __name__ == '__main__':
    main()
