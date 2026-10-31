# DG-PIM

Source code for a cycle-accurate HBM3-PIM attention simulator, numerical checks, and experimental analysis utilities.

## Build

Requires Linux or WSL2, CMake, a C++20 compiler, and Python 3.10 or later. C++ dependencies are included under `native_base/baseline/reproduction/native/native_source/ext/`; the build does not need to download them.

```bash
bash scripts/build_simulator.sh
```

Build outputs are placed under `build/`. To use a different build directory, set `DG_PIM_BUILD_DIR`. To change build parallelism, set `DG_PIM_JOBS`.

## Tests

```bash
bash scripts/check.sh
bash scripts/check.sh --native
```

The native check builds the simulator and runs a deterministic synthetic fixture twice to test basic execution and repeatability. It is not a substitute for the original experimental campaign.

## Code layout

- `native_base/baseline/reproduction/native/native_source/`: simulator and bundled C++ dependencies.
- `native_base/numerics/`: numerical checks and reference implementations.
- `native_base/runtime/`: lifecycle controller variants and auditing code.
- `output/ecp/`: configuration manifests and campaign runners.
- `output/consumer/`: trace and control-analysis utilities.
- `analysis/`: analytical sensitivity and system projections, plus compact summary tables.
- `tools/`: local repository and native smoke checks.

## Data availability

The included task manifests reference original model captures, raw key arrays, and archived native traces that are **not included**. The campaign runners fail when their required inputs are absent. Summary tables and parameterized calculations in `analysis/` do not regenerate those native observations. They must not be treated as independent simulator measurements. To reproduce a full campaign, supply the original captures, pinned model snapshots, and the native records referenced by the manifests.

The repository is source code, not a complete experimental-data archive or a silicon implementation. The project is distributed under its root license; vendored components retain their own license files. See `THIRD_PARTY.md`.

For large native runs, set `DG_PIM_TMP_DIR` to a writable directory with enough space; by default the runner uses `/dev/shm` when available. The analysis utilities use NumPy and the capture runners require additional dependencies specified by their own imports.
