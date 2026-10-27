#!/usr/bin/env bash
# Power the guest off cleanly, falling back to killing QEMU after 60 s.
set -euo pipefail
source "$(dirname "$0")/common.sh"
cd "$VM_DIR"
[[ -f qemu.pid ]] || { echo "not running"; exit 0; }
pid=$(cat qemu.pid)
ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" 'sudo systemctl poweroff' 2>/dev/null || true
for _ in $(seq 1 60); do
    kill -0 "$pid" 2>/dev/null || { rm -f qemu.pid; echo stopped; exit 0; }
    sleep 1
done
kill "$pid" && rm -f qemu.pid && echo killed
