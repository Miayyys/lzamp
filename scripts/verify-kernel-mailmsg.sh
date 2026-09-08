#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kernel=${1:-"$root/linux"}
for source in "$root"/mailmsg/*; do
    test -f "$source" || continue
    cmp "$source" "$kernel/drivers/soc/rockchip/mailmsg/${source##*/}"
done
cmp "$root/scripts/linux-support/dts/rk3588s-lzamp-linux.dts" \
    "$kernel/arch/arm64/boot/dts/rockchip/rk3588s-lzamp-linux.dts"
for source in lzamp_amp_mailmsg.c lzamp_mailmsg_common.c; do
    diff -u <(sed 's|../../../mailmsg/|mailmsg/|g' \
        "$root/scripts/linux-support/drivers/$source") \
        "$kernel/drivers/soc/rockchip/$source"
done
