#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/native_base/baseline/reproduction/native/native_source"
BUILD="${DG_PIM_BUILD_DIR:-$ROOT/build}"
if ! command -v cmake >/dev/null 2>&1; then
  printf '%s\n' 'CMake is required.' >&2
  exit 2
fi
JOBS="${DG_PIM_JOBS:-2}"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DSPDLOG_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TESTS=OFF
cmake --build "$BUILD" --parallel "$JOBS"
printf 'Simulator: %s\nLibrary: %s\n' "$BUILD/ramulator2" "$BUILD/libramulator.so"
