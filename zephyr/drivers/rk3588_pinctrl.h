/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_PINCTRL_H
#define LZAMP_RK3588_PINCTRL_H

#include "rk3588_mmio.h"

enum lzamp_rk3588_pin_profile {
	LZAMP_PIN_PROFILE_I2C7_GPIO = 0,
	LZAMP_PIN_PROFILE_SPI0 = 1,
};

struct lzamp_rk3588_pinctrl {
	uintptr_t ioc_base;
	const struct lzamp_mmio_ops *ops;
};

int lzamp_rk3588_pinctrl_init(struct lzamp_rk3588_pinctrl *pinctrl,
			      uintptr_t ioc_base,
			      const struct lzamp_mmio_ops *ops);
int lzamp_rk3588_pinctrl_uart5(struct lzamp_rk3588_pinctrl *pinctrl);
int lzamp_rk3588_pinctrl_uart7(struct lzamp_rk3588_pinctrl *pinctrl);
int lzamp_rk3588_pinctrl_profile(struct lzamp_rk3588_pinctrl *pinctrl,
				 enum lzamp_rk3588_pin_profile profile);
int lzamp_rk3588_pinctrl_pwm7(struct lzamp_rk3588_pinctrl *pinctrl);

#endif
