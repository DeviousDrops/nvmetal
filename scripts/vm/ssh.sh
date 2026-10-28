#!/usr/bin/env bash
# ssh into the guest, or run a command there: scripts/vm/ssh.sh 'lspci -nn'
set -euo pipefail
source "$(dirname "$0")/common.sh"
exec ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "$@"
