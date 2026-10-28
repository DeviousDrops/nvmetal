#!/usr/bin/env bash
# Copy the working tree into the guest at ~/nvmetal (build dirs excluded), so
# the guest builds from exactly what is on the host.
set -euo pipefail
source "$(dirname "$0")/common.sh"
rsync -a --delete --exclude '/build*' --exclude '.git' \
    -e "ssh ${SSH_OPTS[*]}" "$REPO_DIR/" "$VM_USER@127.0.0.1:nvmetal/"
