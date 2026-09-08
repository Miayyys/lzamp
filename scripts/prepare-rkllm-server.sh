#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Assemble locally from a pinned checkout; no downloads or board operations.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# != 2 ]]; then
    echo 'Usage: bash scripts/prepare-rkllm-server.sh rknn-llm-checkout NEW-output-directory' >&2
    exit 2
fi
src=$(realpath "$1")
out=$2
pin=878f9361fd3afa7e167b7079918918f78d2c1c2a
[[ $(git -C "$src" rev-parse HEAD) == "$pin" ]] || { echo 'Wrong rknn-llm revision' >&2; exit 1; }
[[ ! -e $out ]] || { echo 'Output already exists; choose a new directory' >&2; exit 1; }
mkdir -p "$out/lib"
# Read committed content, not potentially modified working-tree files.
git -C "$src" show "$pin:examples/rkllm_server_demo/rkllm_server/flask_server.py" > "$out/flask_server.py"
git -C "$src" show "$pin:rkllm-runtime/Linux/librkllm_api/aarch64/librkllmrt.so" > "$out/lib/librkllmrt.so"
git -C "$src" show "$pin:LICENSE" > "$out/LICENSE.upstream"
patch --batch --forward -d "$out" -p1 < "$root/agent/rkllm-server-r7.patch"
python3 -m py_compile "$out/flask_server.py"
sha256sum "$out/flask_server.py" "$out/lib/librkllmrt.so"
echo 'Prepared server only. Target still requires Flask, the model and Zephyr image.'
