#!/usr/bin/env bash
# Host-side test matrix (no device needed): plain build, ASan+UBSan, TSan,
# a TSan negative control, and the format check. Logs go to
# bench/results/tests/$TAG-*.txt (TAG defaults to "host"). Exit status = number of failed steps.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=bench/results/tests
TAG=${TAG:-host}
mkdir -p "$OUT"
CC_SAN=${CC_SAN:-clang}
failed=0
step() {
    local name=$1; shift
    if "$@" > "$OUT/$TAG-$name.txt" 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name (see $OUT/$TAG-$name.txt)"; failed=$((failed + 1))
    fi
}

build_and_test() { # dir cc sanitize
    cmake -S . -B "$1" -G Ninja -DCMAKE_C_COMPILER="$2" -DNVMETAL_SANITIZE="$3" &&
        ninja -C "$1" && ctest --test-dir "$1" -L 'unit|fake' --output-on-failure -V
}

step gcc build_and_test build-ci-gcc gcc ""
step clang build_and_test build-ci-clang clang ""
step asan-ubsan build_and_test build-ci-asan "$CC_SAN" address,undefined
step tsan build_and_test build-ci-tsan "$CC_SAN" thread

# Negative control: weaken the SQ doorbell store from release to relaxed in a
# scratch copy. TSan must now report races between the driver's SQE stores
# and the fake device's SQE loads, proving the clean TSan run means something.
negative_control() {
    local d
    d=$(mktemp -d)
    cp -r CMakeLists.txt src include tests tools "$d"/
    sed -i 's/__atomic_store_n(db, v, __ATOMIC_RELEASE);/__atomic_store_n(db, v, __ATOMIC_RELAXED);/' \
        "$d/src/mmio.h"
    cmake -S "$d" -B "$d/b" -G Ninja -DCMAKE_C_COMPILER="$CC_SAN" -DNVMETAL_SANITIZE=thread >/dev/null &&
        ninja -C "$d/b" tests/test_fake >/dev/null || return 1
    NVMETAL_FAKE_CMDS=20000 "$d/b/tests/test_fake" stress_single_qpair > "$d/out" 2>&1
    local rc=$? n
    n=$(grep -c 'WARNING: ThreadSanitizer' "$d/out")
    echo "relaxed doorbell store: exit $rc, $n ThreadSanitizer reports"
    grep -m1 -A3 'WARNING: ThreadSanitizer' "$d/out"
    rm -rf "$d"
    [[ $rc -ne 0 && $n -gt 0 ]]
}
step tsan-negative-control negative_control
# Formatting is defined by clang-format 21 (CI pins 21.1.8); other majors differ.
if clang-format --version 2>/dev/null | grep -q 'version 21\.'; then
    step format scripts/check-format.sh
else
    echo "SKIP format (clang-format 21.x not installed; CI checks it)"
fi

echo "$failed step(s) failed"
exit $failed
