#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "$ROOT/tools/check_repository.py" --root "$ROOT"
python3 -m unittest discover -s "$ROOT/tests" -p 'test_*.py' -v
if [[ "${1:-}" == "--native" ]]; then
  bash "$ROOT/scripts/build_simulator.sh"
  python3 "$ROOT/tools/smoke_native.py" --binary "${DG_PIM_BUILD_DIR:-$ROOT/build}/ramulator2"
fi
