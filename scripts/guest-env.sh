# Sourced inside the guest. Fixed PCI slots come from scripts/vm/run.sh.
NVMDATA_BDF=0000:00:04.0
NVMNULL_BDF=0000:00:05.0

# Kernel block device (e.g. /dev/nvme1n1) of the controller at a BDF, found
# through sysfs because nvmeX numbering changes when controllers are rebound.
nvme_blkdev() {
    local c
    for c in /sys/bus/pci/devices/"$1"/nvme/nvme*; do
        [[ -e $c ]] || continue
        local ns=("$c"/nvme*n1)
        [[ -e ${ns[0]} ]] && { echo "/dev/$(basename "${ns[0]}")"; return 0; }
    done
    return 1
}

# Generic char device (/dev/ngXn1) of the same namespace, for io_uring_cmd.
nvme_chardev() {
    local b
    b=$(nvme_blkdev "$1") || return 1
    echo "/dev/ng${b#/dev/nvme}"
}
