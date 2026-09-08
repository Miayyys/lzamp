#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
lzamp_root=$(cd -- "${script_dir}/.." && pwd)
zephyr_src=${1:-"${lzamp_root}/third_party/zephyrproject/zephyr"}
expected_commit=684c9e8f32e4373a21098559f748f06915f950c9
patch_path="${lzamp_root}/zephyr/patches/0001-arm64-rk3588-map-lzamp-shared-memory.patch"

if [[ ! -d "${zephyr_src}/.git" && ! -f "${zephyr_src}/.git" ]]; then
	printf 'error: not a Git worktree: %s\n' "${zephyr_src}" >&2
	exit 2
fi

actual_commit=$(git -C "${zephyr_src}" rev-parse HEAD)
if [[ "${actual_commit}" != "${expected_commit}" ]]; then
	printf 'error: expected Zephyr commit %s, got %s\n' \
		"${expected_commit}" "${actual_commit}" >&2
	exit 2
fi

if [[ -n "$(git -C "${zephyr_src}" status --porcelain)" ]]; then
	printf 'error: Zephyr worktree is not clean: %s\n' "${zephyr_src}" >&2
	exit 2
fi

git -C "${zephyr_src}" apply --check "${patch_path}"
git -C "${zephyr_src}" apply "${patch_path}"
printf 'prepared Zephyr tree at %s\n' "${zephyr_src}"
