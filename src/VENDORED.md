# Vendored code

| Path | Upstream | Version | License |
|---|---|---|---|
| `NeuralAmpModelerCore/NAM/` | [sdatkinson/NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) | `2563c0f` ("Optimize long linear convolution scheduling (#324)") | MIT (`NeuralAmpModelerCore/LICENSE`) |
| `NeuralAmpModelerCore/Dependencies/eigen/` | [Eigen](https://gitlab.com/libeigen/eigen), NAM core's submodule | `bc3b39870` (before-3.4-1474) | MPL2 (`COPYING.*`) |
| `NeuralAmpModelerCore/Dependencies/nlohmann/` | [nlohmann/json](https://github.com/nlohmann/json) single header, NAM core's copy | as shipped in `2563c0f` | MIT |

Unmodified. Only what the build uses was copied: the core's `NAM/` sources, Eigen's `Eigen/` and
`unsupported/Eigen/` header trees (`NAM/linear.cpp` uses `unsupported/Eigen/FFT`), and `json.hpp`.
The core's other submodule (AudioDSPTools) and its tools, tests and CMake files are left out.

To update: copy the same paths from a newer NeuralAmpModelerCore checkout (with submodules), rebuild,
and re-run the CPU bench from the README before shipping.
