#!/usr/bin/env bash
# Download the Ubuntu 24.04 cloud image and its unpacked kernel/initrd so the
# guest kernel command line can be controlled with -kernel/-initrd/-append.
set -euo pipefail
source "$(dirname "$0")/common.sh"
mkdir -p "$VM_DIR"
cd "$VM_DIR"
fetch() {
    [[ -s "$2" ]] && { echo "have $2"; return; }
    curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}
fetch "$IMG_BASE/noble-server-cloudimg-amd64.img" noble.img
fetch "$IMG_BASE/unpacked/noble-server-cloudimg-amd64-vmlinuz-generic" vmlinuz
fetch "$IMG_BASE/unpacked/noble-server-cloudimg-amd64-initrd-generic" initrd
curl -fsL "$IMG_BASE/SHA256SUMS" -o SHA256SUMS
grep -E ' \*?noble-server-cloudimg-amd64.img$' SHA256SUMS | awk '{print $1"  noble.img"}' | sha256sum -c -
