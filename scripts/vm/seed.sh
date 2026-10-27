#!/usr/bin/env bash
# Create the guest's ssh key, a cloud-init NoCloud seed, a copy-on-write boot
# disk on top of the cloud image, and the raw 2 GiB NVMDATA backing file.
set -euo pipefail
source "$(dirname "$0")/common.sh"
cd "$VM_DIR"
[[ -f noble.img ]] || { echo "run fetch.sh first" >&2; exit 1; }
[[ -f id_ed25519 ]] || ssh-keygen -q -t ed25519 -N '' -C nvmetal-vm -f id_ed25519

cat > user-data <<UD
#cloud-config
hostname: nvmetal
users:
  - name: $VM_USER
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: true
    ssh_authorized_keys:
      - $(cat id_ed25519.pub)
package_update: true
package_upgrade: false
packages: [build-essential, cmake, ninja-build, fio, liburing-dev, nvme-cli, pciutils,
           sysstat, strace, gdb, python3, rsync, jq]
runcmd:
  # perf must match the running kernel exactly; the kernel comes from -kernel.
  - [sh, -c, "apt-get install -y linux-tools-\$(uname -r) linux-tools-generic || true"]
  # Background package timers would add noise to benchmark runs.
  - [systemctl, disable, --now, unattended-upgrades.service, apt-daily.timer, apt-daily-upgrade.timer]
  - [sh, -c, "echo vfio-pci > /etc/modules-load.d/vfio.conf; modprobe vfio-pci"]
UD
printf 'instance-id: nvmetal-1\nlocal-hostname: nvmetal\n' > meta-data
cloud-localds seed.img user-data meta-data

if [[ ! -f disk.qcow2 ]]; then
    qemu-img create -q -f qcow2 -F qcow2 -b noble.img disk.qcow2 20G
fi
if [[ ! -f data.img ]]; then
    truncate -s 2G data.img
fi
echo "seeded $VM_DIR"
