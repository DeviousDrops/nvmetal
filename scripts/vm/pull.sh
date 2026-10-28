#!/usr/bin/env bash
# Copy results produced in the guest (bench/results/) back into the host tree.
set -euo pipefail
source "$(dirname "$0")/common.sh"
mkdir -p "$REPO_DIR/bench/results"
rsync -a -e "ssh ${SSH_OPTS[*]}" "$VM_USER@127.0.0.1:nvmetal/bench/results/" "$REPO_DIR/bench/results/"
