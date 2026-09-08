#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if (( $# != 3 )); then
	printf 'usage: %s UBOOT_OUT RKBIN_SRC OUTPUT\n' "$0" >&2
	exit 2
fi

uboot_out=$(realpath -- "$1")
rkbin_src=$(realpath -- "$2")
output=$(realpath -m -- "$3")
usb471="${uboot_out}/u-boot-rockchip-usb471.bin"
usb472="${uboot_out}/u-boot-rockchip-usb472.bin"
boot_merger="${rkbin_src}/tools/boot_merger"
null0="${rkbin_src}/bin/rk35/rk3588_ramboot_null0.bin"
null1="${rkbin_src}/bin/rk35/rk3588_ramboot_null1.bin"

for input in "${usb471}" "${usb472}" "${boot_merger}" "${null0}" "${null1}"; do
	if [[ ! -f "${input}" ]]; then
		printf 'error: required input not found: %s\n' "${input}" >&2
		exit 2
	fi
done

tmp_dir=$(mktemp -d /tmp/lzamp-maskrom-pack.XXXXXX)
trap 'rm -rf -- "${tmp_dir}"' EXIT

cp "${usb471}" "${tmp_dir}/code471.bin"
cp "${usb472}" "${tmp_dir}/code472.bin"
cp "${null0}" "${tmp_dir}/null0.bin"
cp "${null1}" "${tmp_dir}/null1.bin"

cat >"${tmp_dir}/lzamp-ramboot.ini" <<'EOF'
[CHIP_NAME]
NAME=RK3588
[VERSION]
MAJOR=1
MINOR=0
[CODE471_OPTION]
NUM=1
Path1=code471.bin
Sleep=1
[CODE472_OPTION]
NUM=1
Path1=code472.bin
[LOADER_OPTION]
NUM=2
LOADER1=FlashData
LOADER2=FlashBoot
FlashData=null0.bin
FlashBoot=null1.bin
[OUTPUT]
PATH=lzamp-rk3588s-maskrom-loader.bin
[SYSTEM]
NEWIDB=true
[FLAG]
471_RC4_OFF=true
RC4_OFF=true
EOF

(
	cd "${tmp_dir}"
	"${boot_merger}" lzamp-ramboot.ini
)

mkdir -p -- "$(dirname -- "${output}")"
install -m 0644 "${tmp_dir}/lzamp-rk3588s-maskrom-loader.bin" "${output}"
printf 'packed MaskROM RAM loader: %s\n' "${output}"
printf 'warning: host packaging is verified; board download is not yet authorized or validated\n'
