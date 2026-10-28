#!/usr/bin/env bash
# Guest integration tests. Run inside the VM from the repo root after building:
#   sudo scripts/test_guest.sh [suite...]
# Suites: identify io. Each prints PASS/FAIL lines; the exit status is the
# number of failed suites. Output is also kept under bench/results/tests/.
set -uo pipefail
cd "$(dirname "$0")/.."
source scripts/guest-env.sh
B=${BUILD_DIR:-build}
OUT=bench/results/tests
mkdir -p "$OUT"
failed=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; failed=$((failed + 1)); }

suite_identify() {
    local ok=1
    for bdf in "$NVMDATA_BDF" "$NVMNULL_BDF"; do
        scripts/bind.sh "$bdf" vfio >/dev/null || ok=0
        if "$B/tools/nvmetal-identify" "$bdf" > "$OUT/identify-$bdf.txt" 2>&1 &&
           "$B/tools/nvmetal-identify" --json "$bdf" > "$OUT/identify-$bdf.json" 2>&1; then
            serial=$(jq -r .serial "$OUT/identify-$bdf.json")
            echo "  $bdf serial=$serial ns=$(jq '.namespaces|length' "$OUT/identify-$bdf.json")" \
                 "qpairs=$(jq .qpairs_created "$OUT/identify-$bdf.json")"
        else
            cat "$OUT/identify-$bdf.txt"
            ok=0
        fi
    done
    [[ $(jq -r .serial "$OUT/identify-$NVMDATA_BDF.json") == NVMDATA ]] || ok=0
    [[ $(jq -r .serial "$OUT/identify-$NVMNULL_BDF.json") == NVMNULL ]] || ok=0
    ((ok)) && pass identify || fail identify
}

suite_io() {
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    if "$B/tests/guest/io_smoke" "$NVMDATA_BDF" > "$OUT/io_smoke.txt" 2>&1; then
        cat "$OUT/io_smoke.txt"; pass io
    else
        cat "$OUT/io_smoke.txt"; fail io
    fi
}

suites=("$@")
[[ ${#suites[@]} -eq 0 ]] && suites=(identify io)
for s in "${suites[@]}"; do
    "suite_$s"
done
[[ -n ${SUDO_USER:-} ]] && chown -R "$SUDO_USER:" bench/results
echo "$failed suite(s) failed"
exit $failed
