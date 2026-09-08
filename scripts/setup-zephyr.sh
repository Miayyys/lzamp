#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
workspace="$root/third_party/zephyrproject"
venv="$root/build/zephyr-venv"
commit=684c9e8f32e4373a21098559f748f06915f950c9
for tool in git python3 cmake ninja dtc aarch64-linux-gnu-gcc; do
    command -v "$tool" >/dev/null
done
python3 -m venv "$venv"
"$venv/bin/pip" install 'west==1.5.0'
if [[ ! -e "$workspace/zephyr" ]]; then
    mkdir -p "$workspace"
    git clone --no-checkout https://github.com/zephyrproject-rtos/zephyr.git "$workspace/zephyr"
    git -C "$workspace/zephyr" checkout --detach "$commit"
fi
test "$(git -C "$workspace/zephyr" rev-parse HEAD)" = "$commit"
"$venv/bin/pip" install -r "$workspace/zephyr/scripts/requirements-base.txt"
if [[ ! -d "$workspace/.west" ]]; then
    "$venv/bin/west" init -l "$workspace/zephyr"
fi
patch="$root/zephyr/patches/0001-arm64-rk3588-map-lzamp-shared-memory.patch"
if ! git -C "$workspace/zephyr" apply --reverse --check "$patch" 2>/dev/null; then
    bash "$root/scripts/prepare-zephyr.sh" "$workspace/zephyr"
fi
printf 'Zephyr prepared. Run make zephyr from %s\n' "$root"
