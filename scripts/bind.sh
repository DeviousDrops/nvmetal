#!/usr/bin/env bash
# Rebind a PCI device between vfio-pci and the kernel nvme driver.
#   scripts/bind.sh 0000:00:04.0 vfio|nvme
# Uses driver_override so the choice sticks across re-probes, and checks the
# IOMMU group: VFIO only grants access once every endpoint in the group is
# bound to vfio-pci (or has no driver).
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <bdf> vfio|nvme" >&2; exit 2; }
bdf=$1
case $2 in
vfio) drv=vfio-pci ;;
nvme) drv=nvme ;;
*) echo "unknown driver '$2'" >&2; exit 2 ;;
esac
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

dev=/sys/bus/pci/devices/$bdf
[[ -e $dev ]] || { echo "no such device $bdf" >&2; exit 1; }
modprobe "$drv"

cur=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null || true)
if [[ $cur == "$drv" ]]; then
    echo "$bdf already bound to $drv"
else
    echo "$drv" > "$dev/driver_override"
    if [[ -n $cur ]]; then
        echo "$bdf" > "$dev/driver/unbind"
    fi
    echo "$bdf" > /sys/bus/pci/drivers_probe
    new=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null || true)
    [[ $new == "$drv" ]] || { echo "bind to $drv failed (now '${new:-none}')" >&2; exit 1; }
    echo "$bdf: ${cur:-none} -> $drv"
fi

if [[ $drv == vfio-pci ]]; then
    if [[ -e $dev/iommu_group ]]; then
        group=$(basename "$(readlink "$dev/iommu_group")")
        for other in /sys/kernel/iommu_groups/"$group"/devices/*; do
            o=$(basename "$other")
            [[ $o == "$bdf" ]] && continue
            od=$(basename "$(readlink "$other/driver" 2>/dev/null)" 2>/dev/null || echo none)
            # Bridges may stay bound to pcieport; other endpoints may not.
            class=$(cat "$other/class")
            if [[ $class != 0x0604* && $od != vfio-pci && $od != none ]]; then
                echo "warning: $o in IOMMU group $group is bound to $od;" \
                     "VFIO will refuse the group until it is unbound" >&2
            fi
        done
        node=/dev/vfio/$group
    else
        # No IOMMU: vfio-pci exposes the device only in unsafe no-IOMMU mode.
        group=$(basename "$(readlink "$dev/iommu_group" 2>/dev/null || echo none)")
        node=$(ls /dev/vfio/noiommu-* 2>/dev/null | head -1 || true)
    fi
    for _ in $(seq 1 50); do [[ -e $node ]] && break; sleep 0.1; done
    echo "IOMMU group $group, device node $node"
fi
