#!/usr/bin/env bash
# Boot the guest in the background and wait until ssh answers (and, on first
# boot, until cloud-init has finished installing packages).
set -euo pipefail
source "$(dirname "$0")/common.sh"
cd "$VM_DIR"
if [[ -f qemu.pid ]] && kill -0 "$(cat qemu.pid)" 2>/dev/null; then
    echo "already running (pid $(cat qemu.pid))"
    exit 0
fi

# KVM if usable, else TCG. intremap=on needs the split irqchip under KVM.
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL=(-accel kvm -cpu host -machine q35,kernel-irqchip=split)
    echo kvm > accel
else
    ACCEL=(-accel tcg -cpu max -machine q35)
    echo tcg > accel
fi

APPEND="root=LABEL=cloudimg-rootfs console=ttyS0 intel_iommu=on iommu=pt"
APPEND+=" nvme.poll_queues=${NVMETAL_POLL_QUEUES:-4} hugepagesz=2M hugepages=512"
APPEND+=" ${NVMETAL_EXTRA_APPEND:-}"

# intel-iommu must come before the PCI devices it translates for. The NVMe
# controllers get fixed slots so their BDFs are stable: NVMDATA 00:04.0,
# NVMNULL 00:05.0.
qemu-system-x86_64 "${ACCEL[@]}" -smp 4 -m 4G \
    -device intel-iommu,intremap=on,caching-mode=on \
    -kernel vmlinuz -initrd initrd -append "$APPEND" \
    -drive if=virtio,file=disk.qcow2,format=qcow2 \
    -drive if=virtio,file=seed.img,format=raw \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" -device virtio-net-pci,netdev=n0 \
    -blockdev driver=file,filename=data.img,node-name=dfile \
    -blockdev driver=raw,file=dfile,node-name=data \
    -device nvme,serial=NVMDATA,drive=data,addr=0x4 \
    -blockdev driver=null-co,node-name=null0,size=17179869184,read-zeroes=on \
    -device nvme,serial=NVMNULL,drive=null0,addr=0x5 \
    -display none -serial "file:$VM_DIR/console.log" \
    -pidfile "$VM_DIR/qemu.pid" -daemonize

echo -n "waiting for ssh"
for _ in $(seq 1 180); do
    if ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" true 2>/dev/null; then
        echo " up"
        ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "cloud-init status --wait >/dev/null; cloud-init status"
        exit 0
    fi
    echo -n .
    sleep 2
done
echo " timeout; see $VM_DIR/console.log" >&2
exit 1
