/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_PWM_H
#define LZAMP_RK3588_PWM_H

#include "rk3588_mmio.h"

struct lzamp_rk3588_pwm {
	uintptr_t base;
	const struct lzamp_mmio_ops *ops;
	uint32_t clock_hz;
};

int lzamp_rk3588_pwm_init(struct lzamp_rk3588_pwm *pwm, uintptr_t base,
			  const struct lzamp_mmio_ops *ops, uint32_t clock_hz);
int lzamp_rk3588_pwm_set(struct lzamp_rk3588_pwm *pwm,
			 uint32_t period_ns, uint32_t duty_ns,
			 int inverted);
int lzamp_rk3588_pwm_stop(struct lzamp_rk3588_pwm *pwm);

#endif
