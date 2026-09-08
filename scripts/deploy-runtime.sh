#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Upload userspace entry points only; does not start services or replace firmware.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# != 1 || $1 == -* ]]; then
    echo 'Usage: bash scripts/deploy-runtime.sh user@board-ip' >&2
    exit 2
fi
target=$1
ssh -4 "$target" 'mkdir -p /userdata/lzamp/agent /userdata/lzamp/bin'
scp -4 "$root/agent/mailmsg_agent.py" "$target:/userdata/lzamp/agent/mailmsg_agent-v7.py"
scp -4 "$root/scripts/lzamp-runtime" "$target:/userdata/lzamp/bin/lzamp-runtime"
ssh -4 "$target" 'chmod +x /userdata/lzamp/bin/lzamp-runtime; sha256sum /userdata/lzamp/agent/mailmsg_agent-v7.py'
