# Third-party code

The simulator includes source derived from Ramulator and bundles dependencies for offline builds. The original license text is preserved in the respective directories:

| Component | Location | License file |
| --- | --- | --- |
| Ramulator simulator base | `native_base/baseline/reproduction/native/native_source/` | `LICENSE` |
| yaml-cpp | `.../native_source/ext/yaml-cpp/` | `LICENSE` |
| spdlog | `.../native_source/ext/spdlog/` | `LICENSE` |
| argparse | `.../native_source/ext/argparse/` | `LICENSE` |

Dependency revisions are pinned in the existing CMake configuration. Do not remove the dependency metadata or license notices when redistributing.
