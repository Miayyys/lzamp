/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_RESET_H
#define LZAMP_RK3588_RESET_H

#include "rk3588_mmio.h"

enum lzamp_rk3588_reset {
	LZAMP_RESET_UART5,
	LZAMP_RESET_UART7,
	LZAMP_RESET_I2C7,
	LZAMP_RESET_SPI0,
	LZAMP_RESET_PWM1,
	LZAMP_RESET_SARADC,
};

struct lzamp_rk3588_reset_controller {
	uintptr_t cru_base;
	const struct lzamp_mmio_ops *ops;
};

int lzamp_rk3588_reset_init(struct lzamp_rk3588_reset_controller *reset,
			    uintptr_t cru_base,
			    const struct lzamp_mmio_ops *ops);
int lzamp_rk3588_reset_pulse(struct lzamp_rk3588_reset_controller *reset,
			     enum lzamp_rk3588_reset peripheral);

#endif
