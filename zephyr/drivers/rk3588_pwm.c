/* SPDX-License-Identifier: MIT */
#include "rk3588_pwm.h"

#include <stddef.h>

/* RK3588 uses the rk3328-compatible PWM v3 channel register layout. */
#define PWM_PERIOD 0x04U
#define PWM_DUTY   0x08U
#define PWM_CTRL   0x0cU
#define PWM_ENABLE            (1U << 0)
#define PWM_CONTINUOUS        (1U << 1)
#define PWM_DUTY_POSITIVE     (1U << 3)
#define PWM_INACTIVE_POSITIVE (1U << 4)
#define PWM_LOCK              (1U << 6)

int lzamp_rk3588_pwm_init(struct lzamp_rk3588_pwm *pwm, uintptr_t base,
			  const struct lzamp_mmio_ops *ops, uint32_t clock_hz)
{
	if (!pwm || !ops || !ops->read32 || !ops->write32 || !ops->delay_us ||
	    !clock_hz)
		return -1;
	pwm->base = base;
	pwm->ops = ops;
	pwm->clock_hz = clock_hz;
	return lzamp_rk3588_pwm_stop(pwm);
}

int lzamp_rk3588_pwm_set(struct lzamp_rk3588_pwm *pwm,
			 uint32_t period_ns, uint32_t duty_ns,
			 int inverted)
{
	uint64_t period_ticks, duty_ticks;
	uint32_t ctrl;

	if (!pwm || !pwm->ops || !period_ns || duty_ns > period_ns)
		return -1;
	period_ticks = ((uint64_t)pwm->clock_hz * period_ns + 500000000ULL) /
		       1000000000ULL;
	duty_ticks = ((uint64_t)pwm->clock_hz * duty_ns + 500000000ULL) /
		     1000000000ULL;
	if (!period_ticks || period_ticks > 0xffffffffULL ||
	    duty_ticks > period_ticks)
		return -1;
	ctrl = pwm->ops->read32(pwm->base + PWM_CTRL);
	ctrl |= PWM_LOCK;
	pwm->ops->write32(ctrl, pwm->base + PWM_CTRL);
	pwm->ops->write32((uint32_t)period_ticks, pwm->base + PWM_PERIOD);
	pwm->ops->write32((uint32_t)duty_ticks, pwm->base + PWM_DUTY);
	ctrl &= ~(PWM_DUTY_POSITIVE | PWM_INACTIVE_POSITIVE | PWM_LOCK);
	ctrl |= PWM_ENABLE | PWM_CONTINUOUS;
	ctrl |= inverted ? PWM_INACTIVE_POSITIVE : PWM_DUTY_POSITIVE;
	pwm->ops->delay_us(1U);
	pwm->ops->write32(ctrl, pwm->base + PWM_CTRL);
	return 0;
}

int lzamp_rk3588_pwm_stop(struct lzamp_rk3588_pwm *pwm)
{
	uint32_t ctrl;

	if (!pwm || !pwm->ops)
		return -1;
	ctrl = pwm->ops->read32(pwm->base + PWM_CTRL);
	ctrl &= ~PWM_ENABLE;
	pwm->ops->write32(ctrl, pwm->base + PWM_CTRL);
	return 0;
}
