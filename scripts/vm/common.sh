# Shared settings for the VM scripts. Sourced, not executed.
VM_DIR="${NVMETAL_VM_DIR:-$HOME/vms/nvmetal}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SSH_PORT="${NVMETAL_SSH_PORT:-10022}"
VM_USER=nvm
IMG_BASE=https://cloud-images.ubuntu.com/noble/current
SSH_KEY="$VM_DIR/id_ed25519"
SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
