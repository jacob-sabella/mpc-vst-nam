#!/usr/bin/env bash
# Build NAM (Neural Amp Modeler) as a VST2 effect for the MPC OS plugin host (armhf, RK3288 Cortex-A17).
#   vst/build/nam_vst.so              -> the directory in pluginlist-entry.xml's file="..." on the device
#   vst/build/pluginlist-entry.xml    the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/bench                   on-device CPU benchmark (see README "CPU")
# Models, cab IRs, favorites and the models-removed folder all live next to the .so: see
# models_dir() / cabs_dir() in nam_vst.cpp.
#
#   vst/build.sh          armhf build in arm32v7/gcc:11-bullseye (glibc 2.31) under Docker (what CI runs)
#   ARM_TC=<toolchain> vst/build.sh
#                         same build with a local armv7 hard-float cross toolchain (bootlin
#                         armv7-eabihf--glibc--stable): much faster than emulation
#   vst/build.sh host     x86 build + vst/build/host_test for an offline sanity check
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p vst/build

CORE=src/NeuralAmpModelerCore
INC="-Ivst -I$CORE -I$CORE/NAM -I$CORE/Dependencies/eigen -I$CORE/Dependencies/nlohmann"
SRC="vst/nam_vst.cpp vst/t3k.cpp $CORE/NAM/*.cpp $CORE/NAM/wavenet/*.cpp"
# NAM_USE_INLINE_GEMM: the core's hand-written small-matrix kernels -- Eigen's general GEMM setup
# dominates at A2 channel counts (3/8), ~2x slower on this CPU.
# NAM_SAMPLE_FLOAT: keep the NAM I/O path in float (armv7 NEON has no double SIMD).
# PLUG_VERSION: the VST2 version integer, from the git tag (scripts/version.py).
DEFS="-std=c++20 -O3 -DNDEBUG -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM -DPLUG_VERSION=$(python3 scripts/version.py --vst)"
# -ffast-math lets GCC use NEON for float loops at all (NEON flushes denormals, so without it GCC
# won't vectorize); -mtune=cortex-a17 = the RK3288's core.
ARM="${NAM_FASTMATH--ffast-math} -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard"
LINK="-fPIC -shared -static-libstdc++ -static-libgcc -pthread -Wl,--no-undefined -Wl,--exclude-libs,ALL -fvisibility=hidden"

if [ "${1:-}" = host ]; then
  ${CXX:-clang++} $DEFS -fPIC -shared $INC $SRC -ldl -o vst/build/nam_vst.host.so
  ${CC:-cc} -O2 -o vst/build/host_test vst/host_test.c -ldl -lm
  vst/build/host_test vst/build/nam_vst.host.so
  exit
fi

python3 vst/gen_entry.py

if [ -n "${ARM_TC:-}" ]; then
  X="$ARM_TC/bin/arm-linux-"
  "${X}g++" $DEFS $ARM $LINK $INC $SRC -ldl -o vst/build/nam_vst.so
  "${X}gcc" -O2 $ARM -o vst/build/bench vst/bench.c -ldl -lm
  "${X}strip" vst/build/nam_vst.so vst/build/bench
  "${X}readelf" --dyn-syms -W vst/build/nam_vst.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
else
  docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b \
    -e DEFS="$DEFS" -e ARM="$ARM" -e LINK="$LINK" -e INC="$INC" -e SRC="$SRC" arm32v7/gcc:11-bullseye bash -euc '
    g++ $DEFS $ARM $LINK $INC $SRC -ldl -o vst/build/nam_vst.so
    gcc -O2 $ARM -o vst/build/bench vst/bench.c -ldl -lm
    strip vst/build/nam_vst.so vst/build/bench
    echo "-- exported --"; readelf --dyn-syms -W vst/build/nam_vst.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
    echo "-- needed --"; readelf -d vst/build/nam_vst.so | grep NEEDED
    echo "-- highest glibc (catalog limit 2.32: MPC OS 2.x) --"; readelf -V vst/build/nam_vst.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  '
fi
md5sum vst/build/nam_vst.so
