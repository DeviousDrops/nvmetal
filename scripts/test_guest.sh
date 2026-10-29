#!/usr/bin/env bash
# Guest integration tests. Run inside the VM from the repo root after building:
#   sudo scripts/test_guest.sh [suite...]
# Suites: identify io verify selftest oob crosscheck restart. Each prints PASS/FAIL lines; the exit status is the
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

dmar_faults() { dmesg | grep -c -E 'DMAR: \[DMA (Read|Write)|DMAR:.*fault' || true; }

suite_verify() {
    # >= 1 GiB written and verified, 4 threads, I/O sizes 4 KiB..MDTS, QD 32.
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    local f0 f1
    f0=$(dmar_faults)
    "$B/tools/nvmetal-verify" --bytes 1G --threads 4 --qd 32 --json "$NVMDATA_BDF" \
        > "$OUT/verify.json" 2> "$OUT/verify.err"
    local rc=$?
    f1=$(dmar_faults)
    cat "$OUT/verify.json" "$OUT/verify.err"
    if [[ $rc -eq 0 && $(jq -r .result "$OUT/verify.json") == pass &&
          $(jq .mismatches "$OUT/verify.json") -eq 0 &&
          $(jq .bytes_verified "$OUT/verify.json") -ge 1073741824 &&
          $(jq '.queue_depth_multiple > 10' "$OUT/verify.json") == true && $f1 -eq $f0 ]]; then
        pass verify
    else
        echo "  rc=$rc dmar faults $f0 -> $f1"; fail verify
    fi
}

suite_selftest() {
    # The verifier must catch planted CRC, misdirected and stale blocks.
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    "$B/tools/nvmetal-verify" --bytes 64M --threads 2 --region 256M --inject 3 "$NVMDATA_BDF" \
        > "$OUT/verify-selftest.txt" 2>&1
    local rc=$?
    tail -2 "$OUT/verify-selftest.txt"
    [[ $rc -eq 0 ]] && pass selftest || fail selftest
}

suite_oob() {
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    "$B/tools/nvmetal-verify" --oob "$NVMDATA_BDF" > "$OUT/oob.txt" 2>&1
    local rc=$?
    cat "$OUT/oob.txt"
    [[ $rc -eq 0 ]] && pass oob || fail oob
}

wait_blkdev() {
    local d
    for _ in $(seq 1 100); do
        d=$(nvme_blkdev "$1") && [[ -b $d ]] && { echo "$d"; return 0; }
        sleep 0.1
    done
    return 1
}

suite_crosscheck() {
    # nvmetal writes, the kernel reads; then the kernel writes, nvmetal reads.
    local log="$OUT/crosscheck.txt" ok=1 dev exp got
    : > "$log"
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    "$B/tools/nvmetal-verify" --write-pattern --offset 256M --length 64M --seed 42 \
        "$NVMDATA_BDF" 2>> "$log" || ok=0
    exp=$("$B/tools/nvmetal-verify" --gen-pattern --offset 256M --length 64M --seed 42 |
          sha256sum | cut -d' ' -f1)
    scripts/bind.sh "$NVMDATA_BDF" nvme >> "$log"
    dev=$(wait_blkdev "$NVMDATA_BDF") || { echo "no block device" >> "$log"; ok=0; }
    got=$(dd if="$dev" iflag=direct bs=1M skip=256 count=64 status=none | sha256sum | cut -d' ' -f1)
    echo "nvmetal->kernel: expected $exp kernel read $got ($dev)" >> "$log"
    [[ -n $exp && $exp == "$got" ]] || ok=0

    local tmp
    tmp=$(mktemp)
    head -c 64M /dev/urandom > "$tmp"
    exp=$(sha256sum < "$tmp" | cut -d' ' -f1)
    dd if="$tmp" of="$dev" oflag=direct bs=1M seek=512 conv=fsync status=none || ok=0
    rm -f "$tmp"
    scripts/bind.sh "$NVMDATA_BDF" vfio >> "$log"
    got=$("$B/tools/nvmetal-verify" --dump --offset 512M --length 64M "$NVMDATA_BDF" 2>> "$log" |
          sha256sum | cut -d' ' -f1)
    echo "kernel->nvmetal: kernel wrote $exp nvmetal read $got" >> "$log"
    [[ $exp == "$got" ]] || ok=0
    cat "$log"
    ((ok)) && pass crosscheck || fail crosscheck
}

suite_restart() {
    # open, verify a little, close: 50 times, every one clean.
    scripts/bind.sh "$NVMDATA_BDF" vfio >/dev/null
    local log="$OUT/restart.txt" bad=0 f0 f1
    : > "$log"
    f0=$(dmar_faults)
    for i in $(seq 1 50); do
        if "$B/tools/nvmetal-verify" --bytes 8M --threads 1 --qd 16 --region 64M --seed "$i" \
            "$NVMDATA_BDF" >> "$log" 2>&1; then
            :
        else
            echo "iteration $i failed" >> "$log"; bad=$((bad + 1))
        fi
    done
    f1=$(dmar_faults)
    "$B/tools/nvmetal-identify" "$NVMDATA_BDF" > /dev/null 2>> "$log" || bad=$((bad + 1))
    echo "50 iterations, $bad failed, $(grep -c ': PASS$' "$log") PASS lines, DMAR faults $f0 -> $f1" | tee -a "$log"
    [[ $bad -eq 0 && $f1 -eq $f0 ]] && pass restart || fail restart
}

suites=("$@")
[[ ${#suites[@]} -eq 0 ]] && suites=(identify io verify selftest oob crosscheck restart)
for s in "${suites[@]}"; do
    "suite_$s"
done
[[ -n ${SUDO_USER:-} ]] && chown -R "$SUDO_USER:" bench/results
echo "$failed suite(s) failed"
exit $failed
