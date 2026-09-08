/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_SARADC_H
#define LZAMP_RK3588_SARADC_H

#include "rk3588_mmio.h"

struct lzamp_rk3588_saradc {
	uintptr_t base;
	const struct lzamp_mmio_ops *ops;
};

int lzamp_rk3588_saradc_init(struct lzamp_rk3588_saradc *adc,
			     uintptr_t base, const struct lzamp_mmio_ops *ops);
int lzamp_rk3588_saradc_read(struct lzamp_rk3588_saradc *adc,
			     uint32_t channel, uint32_t *sample,
			     uint32_t timeout_us);

#endif
