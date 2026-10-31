#!/usr/bin/env python3
"""Check structural integrity and report inputs needed by registered tasks."""
import argparse
import ast
import json
from pathlib import Path

EXPECTED = (
    'LICENSE', 'README.md', 'scripts/build_simulator.sh',
    'native_base/baseline/reproduction/native/native_source/CMakeLists.txt',
    'native_base/baseline/reproduction/native/native_source/ext/yaml-cpp/LICENSE',
    'native_base/baseline/reproduction/native/native_source/ext/spdlog/LICENSE',
    'native_base/baseline/reproduction/native/native_source/ext/argparse/LICENSE',
)

def inspect_repo(root: Path):
    missing = [f for f in EXPECTED if not (root / f).is_file()]
    issues = []
    for p in root.rglob('*.py'):
        if any(part.startswith('.') or part == 'build' for part in p.relative_to(root).parts):
            continue
        try:
            ast.parse(p.read_text(encoding='utf-8'), filename=str(p))
        except (SyntaxError, UnicodeError) as exc:
            issues.append(f'{p.relative_to(root)}: {exc}')
    json_files = sorted(p for p in root.rglob('*.json') if 'build' not in p.relative_to(root).parts)
    for p in json_files:
        try:
            json.loads(p.read_text(encoding='utf-8'))
        except (json.JSONDecodeError, UnicodeError) as exc:
            issues.append(f'{p.relative_to(root)}: {exc}')
    manifest = root / 'output/ecp/ALL_REGISTERED_TASKS.json'
    total = 0
    unavailable = set()
    if manifest.is_file():
        jobs = json.loads(manifest.read_text())
        total = len(jobs)
        for job in jobs:
            for key in ('input', 'raw_keys', 'reference'):
                value = job.get(key)
                if not value:
                    issues.append(f'task {job.get("name", "?")} has no {key}')
                elif not Path(value).is_absolute() and not (root / value).is_file():
                    unavailable.add(value)
                elif Path(value).is_absolute() and not Path(value).is_file():
                    unavailable.add(value)
    return {'missing_required_files': missing, 'source_errors': issues,
            'python_files': len(list(root.rglob('*.py'))),
            'json_files': len(json_files), 'registered_tasks': total,
            'external_inputs_missing': len(unavailable),
            'sample_missing_inputs': sorted(unavailable)[:8]}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--strict-fixtures', action='store_true', help='Fail if experiment inputs are absent')
    args = parser.parse_args()
    report = inspect_repo(args.root.resolve())
    print(json.dumps(report, indent=2))
    if report['missing_required_files'] or report['source_errors']:
        raise SystemExit(1)
    if args.strict_fixtures and report['external_inputs_missing']:
        raise SystemExit(2)

if __name__ == '__main__':
    main()
