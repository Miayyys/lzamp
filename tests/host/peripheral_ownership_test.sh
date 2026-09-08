#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

lzamp_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
manifest="${lzamp_root}/config/peripheral-ownership.yaml"
linux_dts="${lzamp_root}/scripts/linux-support/dts/rk3588s-lzamp-linux.dts"
zephyr_overlay="${lzamp_root}/zephyr/app/boards/roc_rk3588_pc.overlay"

grep -Fq 'state: mixed-readiness' "${manifest}"
test "$(grep -Fc 'zephyr_driver: polling-input-output-host-tested-board-pending' "${manifest}")" -eq 3
test "$(grep -Fc 'zephyr_driver: polling-host-tested-board-pending' "${manifest}")" -eq 3
grep -Fq 'zephyr_driver: polling-profile-host-tested-board-pending' "${manifest}"
grep -Fq 'default: i2c7-gpio' "${manifest}"
grep -Fq 'switch_policy: boot-time-linux-and-zephyr-dt-pair' "${manifest}"
grep -Fq 'ownership: controller-bank-exclusive' "${manifest}"
grep -Fq 'zephyr_driver: pwm7-polling-host-tested-electrical-board-pending' "${manifest}"
grep -Fq 'policy: mixed-exclusive' "${manifest}"
grep -Fq 'ownership: line-exclusive-controller-shared' "${manifest}"
grep -Fq 'ownership: controller-exclusive' "${manifest}"
grep -Fq 'ownership: controller-exclusive-reserved' "${manifest}"
grep -Fq 'reserved_lines: [16, 17, 20, 21, 25, 26, 27, 28, 29]' "${manifest}"
grep -Fq 'direct_gpio_lines: [25, 28, 29]' "${manifest}"
grep -Fq 'assigned_leaf_trees: [uart5, uart7, i2c7, spi0, pwm1, saradc]' "${manifest}"
grep -Fq 'write_policy: rockchip-hiword-mask-only' "${manifest}"
grep -Fq 'gpio-reserved-ranges = <16 2>, <20 2>, <25 5>;' "${linux_dts}"
grep -Fq '&uart5 {' "${linux_dts}"
grep -Fq '&uart7 {' "${linux_dts}"
grep -Fq '&i2c7 {' "${linux_dts}"
grep -Fq '&uart9 {' "${linux_dts}"
grep -Fq '&spi0 {' "${linux_dts}"
for node in pwm4 pwm5 pwm6 pwm7 saradc; do
	grep -Fq "&${node} {" "${linux_dts}"
done
grep -Fq '&es8388_sound {' "${linux_dts}"
grep -Fq '/delete-property/ io-channels;' "${linux_dts}"
grep -Fq '/delete-node/ play-pause-key;' "${linux_dts}"
grep -Fq 'lzamp_gpio3: gpio@fec40000' "${zephyr_overlay}"
grep -Fq 'lzamp_uart5: serial@feb80000' "${zephyr_overlay}"
grep -Fq 'lzamp_uart7: serial@feba0000' "${zephyr_overlay}"
grep -Fq 'lzamp_i2c7: i2c@fec90000' "${zephyr_overlay}"
grep -Fq 'lzamp_spi0: spi@feb00000' "${zephyr_overlay}"
for channel in 4 5 6 7; do
	grep -Fq "lzamp_pwm${channel}: pwm@" "${zephyr_overlay}"
done
grep -Fq 'lzamp_saradc: adc@fec10000' "${zephyr_overlay}"
if sed -n '/lzamp_gpio3: gpio@fec40000 {/,/};/p' "${zephyr_overlay}" | grep -Fq 'interrupts ='; then
	printf '%s\n' 'error: shared GPIO3 skeleton must remain polling-only' >&2
	exit 1
fi

printf '%s\n' 'peripheral ownership test: pass'
