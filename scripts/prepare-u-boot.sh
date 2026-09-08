#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
lzamp_root=$(cd -- "${script_dir}/.." && pwd)
uboot_src=${1:-"${lzamp_root}/third_party/u-boot"}
expected_commit=ece349ade2973e220f524ce59e59711cc919263f
patch_dir="${lzamp_root}/scripts/boot/support/patches/upstream-2026.07"

if [[ ! -d "${uboot_src}/.git" && ! -f "${uboot_src}/.git" ]]; then
	printf 'error: not a Git worktree: %s\n' "${uboot_src}" >&2
	exit 2
fi

actual_commit=$(git -C "${uboot_src}" rev-parse HEAD)
if [[ "${actual_commit}" != "${expected_commit}" ]]; then
	printf 'error: expected U-Boot commit %s, got %s\n' \
		"${expected_commit}" "${actual_commit}" >&2
	exit 2
fi

if [[ -n "$(git -C "${uboot_src}" status --porcelain)" ]]; then
	printf 'error: U-Boot worktree is not clean: %s\n' "${uboot_src}" >&2
	exit 2
fi

for patch in \
	"${patch_dir}/0001-arm64-dts-add-lzamp-rk3588s-board.patch" \
	"${patch_dir}/0002-host-pylibfdt-use-python3-api.patch" \
	"${patch_dir}/0003-configs-rk3588-serial-console-fallback.patch" \
	"${patch_dir}/0004-configs-lzamp-default-to-non-global-bootflow.patch" \
	"${patch_dir}/0005-configs-lzamp-network-then-emmc-fallback.patch"
do
	git -C "${uboot_src}" apply --check "${patch}"
	git -C "${uboot_src}" apply "${patch}"
done

printf 'prepared upstream U-Boot v2026.07 tree at %s\n' "${uboot_src}"
printf 'next: make -C LZAMP u-boot-configure u-boot-build\n'
