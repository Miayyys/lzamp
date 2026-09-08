#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
lzamp_root=$(cd -- "${script_dir}/.." && pwd)

usage()
{
	cat <<'EOF'
Usage:
  serve-linux-6.12-tftp.sh [--print-only] IMAGE DTB [BIND_ADDR]

The default bind address is 10.42.0.1:69.  The normal mode creates a
temporary, isolated TFTP root and runs in.tftpd in the foreground.  It does
not stop an existing TFTP service, change networking, access the board, or
write eMMC.
EOF
}

print_only=0
if [[ ${1:-} == --print-only ]]; then
	print_only=1
	shift
fi

if (( $# < 2 || $# > 3 )); then
	usage >&2
	exit 2
fi

image=$(realpath -- "$1")
dtb=$(realpath -- "$2")
pxe_config="${lzamp_root}/u-boot/pxelinux.cfg/default"
bind_addr=${3:-10.42.0.1:69}

for input in "$image" "$dtb"; do
	if [[ ! -f $input ]]; then
		printf 'error: regular file not found: %s\n' "$input" >&2
		exit 2
	fi
done

if [[ ! -f $pxe_config ]]; then
	printf 'error: PXE configuration not found: %s\n' "$pxe_config" >&2
	exit 2
fi

port=${bind_addr##*:}
if [[ ! $port =~ ^[0-9]+$ ]] || (( port < 1 || port > 65535 )); then
	printf 'error: invalid bind address/port: %s\n' "$bind_addr" >&2
	exit 2
fi

image_size=$(stat -c '%s' "$image")
dtb_size=$(stat -c '%s' "$dtb")
image_sha=$(sha256sum "$image")
image_sha=${image_sha%% *}
dtb_sha=$(sha256sum "$dtb")
dtb_sha=${dtb_sha%% *}

printf 'Image source: %s\n' "$image"
printf 'Image size:   %s\n' "$image_size"
printf 'Image SHA256: %s\n' "$image_sha"
printf 'DTB source:   %s\n' "$dtb"
printf 'DTB size:     %s\n' "$dtb_size"
printf 'DTB SHA256:   %s\n' "$dtb_sha"
printf 'PXE config:   %s\n' "$pxe_config"
printf '\nU-Boot RAM-only commands:\n'
printf 'tftpboot 0x04000000 Image\n'
printf 'crc32 0x04000000 ${filesize}\n'
printf 'tftpboot 0x08300000 rk3588s-lzamp-linux.dtb\n'
printf 'crc32 0x08300000 ${filesize}\n'
printf 'booti 0x04000000 - 0x08300000\n'

if (( print_only )); then
	exit 0
fi

if ! command -v in.tftpd >/dev/null 2>&1; then
	printf 'error: in.tftpd not found; install tftp-hpa first\n' >&2
	exit 127
fi

if command -v ss >/dev/null 2>&1 &&
	ss -H -lun 2>/dev/null | grep -Eq "(^|:)${port}([[:space:]]|$)"; then
	printf 'error: UDP port %s is already in use; inspect it before stopping anything\n' "$port" >&2
	exit 71
fi

tmp_root=$(mktemp -d /tmp/lzamp-tftp.XXXXXX)
cleanup()
{
	rm -rf -- "$tmp_root"
}
trap cleanup EXIT INT TERM

install -m 0644 -- "$image" "$tmp_root/Image"
install -m 0644 -- "$dtb" "$tmp_root/rk3588s-lzamp-linux.dtb"
mkdir -p -- "$tmp_root/pxelinux.cfg"
install -m 0644 -- "$pxe_config" "$tmp_root/pxelinux.cfg/default"
printf '\nTFTP root: %s\n' "$tmp_root"
printf 'Serving %s on %s; press Ctrl-C to stop.\n' \
	'Image, rk3588s-lzamp-linux.dtb and pxelinux.cfg/default' "$bind_addr"

if (( EUID == 0 )); then
	in.tftpd --foreground --secure --verbose \
		--address "$bind_addr" "$tmp_root"
else
	sudo in.tftpd --foreground --secure --verbose \
		--address "$bind_addr" "$tmp_root"
fi
