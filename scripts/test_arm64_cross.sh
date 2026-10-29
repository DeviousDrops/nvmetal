#!/usr/bin/env bash
# Cross-compile for arm64 and run the unit and fake-controller tests under
# qemu-user. Exercises the AArch64 barrier code (dmb oshst/oshld/osh) on an
# x86 machine. Needs gcc-aarch64-linux-gnu, libc6-dev-arm64-cross, qemu-user.
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-arm64}
cat > /tmp/nvmetal-arm64.cmake <<'TC'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/aarch64-linux-gnu)
TC
cmake -S . -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=/tmp/nvmetal-arm64.cmake
ninja -C "$B"
# Show that the AArch64 barriers really are in the generated code.
aarch64-linux-gnu-objdump -d "$B/CMakeFiles/nvmetal.dir/src/qpair.c.o" | grep -c -E 'dmb\s+(oshst|oshld|osh)\b' |
    xargs echo "dmb osh* instructions in qpair.o:"
NVMETAL_FAKE_CMDS=${NVMETAL_FAKE_CMDS:-200000} ctest --test-dir "$B" -L 'unit|fake' --output-on-failure
