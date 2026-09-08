#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Publish explicitly selected build files; does not alter U-Boot or partitions.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo 'Usage: bash scripts/deploy-tftp.sh Image board.dtb /srv/tftp' >&2
    exit 2
fi
test -f "$1"
test -f "$2"
test -d "$3"
install -m 644 -- "$1" "$3/Image.new"
install -m 644 -- "$2" "$3/rk3588s-lzamp-linux.dtb.new"
mv -- "$3/Image.new" "$3/Image"
mv -- "$3/rk3588s-lzamp-linux.dtb.new" "$3/rk3588s-lzamp-linux.dtb"
sha256sum "$3/Image" "$3/rk3588s-lzamp-linux.dtb"
