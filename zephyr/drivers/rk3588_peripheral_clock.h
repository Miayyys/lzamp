/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_PERIPHERAL_CLOCK_H
#define LZAMP_RK3588_PERIPHERAL_CLOCK_H

#include <stdint.h>

struct lzamp_rk3588_cru_ops {
	void (*write32)(uint32_t value, uintptr_t address);
};

struct lzamp_rk3588_cru {
	uintptr_t base;
	const struct lzamp_rk3588_cru_ops *ops;
};

enum lzamp_rk3588_peripheral_clock {
	LZAMP_RK3588_CLOCK_UART5,
	LZAMP_RK3588_CLOCK_UART7,
	LZAMP_RK3588_CLOCK_I2C7,
	LZAMP_RK3588_CLOCK_SPI0,
	LZAMP_RK3588_CLOCK_PWM1,
	LZAMP_RK3588_CLOCK_SARADC,
};

enum lzamp_rk3588_clock_result {
	LZAMP_RK3588_CLOCK_OK = 0,
	LZAMP_RK3588_CLOCK_INVALID = -1,
};

int lzamp_rk3588_cru_init(struct lzamp_rk3588_cru *cru, uintptr_t base,
			  const struct lzamp_rk3588_cru_ops *ops);

/*
 * Configure only the instance-specific leaf fields allocated to Zephyr.
 * UART, SPI0 and PWM1 use the 24 MHz oscillator directly; I2C7 uses the
 * 100 MHz parent.  SARADC derives 1 MHz from the 24 MHz oscillator.
 * This API intentionally does not modify a PLL or issue peripheral resets.
 */
int lzamp_rk3588_peripheral_clock_enable(
	struct lzamp_rk3588_cru *cru,
	enum lzamp_rk3588_peripheral_clock peripheral);

int lzamp_rk3588_peripheral_clock_disable(
	struct lzamp_rk3588_cru *cru,
	enum lzamp_rk3588_peripheral_clock peripheral);

#endif
