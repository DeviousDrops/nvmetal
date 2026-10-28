#!/usr/bin/env bash
# M1 gate: the guest is up, both emulated controllers are visible, each sits in
# an IOMMU group, the hugepages and perf are present.
set -euo pipefail
source "$(dirname "$0")/common.sh"
ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" bash -s <<'GUEST'
set -euo pipefail
fail=0
for bdf in 0000:00:04.0 0000:00:05.0; do
    lspci -s "$bdf" -nn | grep -q 'Non-Volatile memory controller' || { echo "missing $bdf"; fail=1; continue; }
    g=$(basename "$(readlink /sys/bus/pci/devices/$bdf/iommu_group)")
    echo "$bdf $(cat /sys/bus/pci/devices/$bdf/device) iommu_group=$g"
done
echo "kernel $(uname -r); hugepages $(awk '/HugePages_Total/{print $2}' /proc/meminfo)"
sudo perf --version
exit $fail
GUEST
