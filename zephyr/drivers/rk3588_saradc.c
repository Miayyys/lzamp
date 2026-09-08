/* SPDX-License-Identifier: MIT */
#include "rk3588_saradc.h"

#include <stddef.h>

#define SARADC_CONV_CON   0x000U
#define SARADC_T_PD_SOC   0x004U
#define SARADC_T_DAS_SOC  0x00cU
#define SARADC_END_INT_EN 0x104U
#define SARADC_END_INT_ST 0x110U
#define SARADC_DATA(ch)   (0x120U + (ch) * 4U)
#define SARADC_END_BIT    (1U << 0)
#define SARADC_START      (1U << 4)
#define SARADC_SINGLE     (1U << 5)
#define SARADC_CH_MASK    0xfU

int lzamp_rk3588_saradc_init(struct lzamp_rk3588_saradc *adc,
			     uintptr_t base, const struct lzamp_mmio_ops *ops)
{
	if (!adc || !ops || !ops->read32 || !ops->write32 || !ops->delay_us)
		return -1;
	adc->base = base;
	adc->ops = ops;
	ops->write32(0x20U, base + SARADC_T_PD_SOC);
	ops->write32(0x0cU, base + SARADC_T_DAS_SOC);
	ops->write32(SARADC_END_BIT, base + SARADC_END_INT_ST);
	return 0;
}

int lzamp_rk3588_saradc_read(struct lzamp_rk3588_saradc *adc,
			     uint32_t channel, uint32_t *sample,
			     uint32_t timeout_us)
{
	uint32_t fields, value;
	int complete = 0;

	if (!adc || !adc->ops || !sample || channel >= 8U)
		return -1;
	adc->ops->write32(SARADC_END_BIT, adc->base + SARADC_END_INT_ST);
	adc->ops->write32((SARADC_END_BIT << 16U) | SARADC_END_BIT,
			  adc->base + SARADC_END_INT_EN);
	fields = SARADC_START | SARADC_SINGLE | channel;
	adc->ops->write32(((SARADC_START | SARADC_SINGLE | SARADC_CH_MASK) << 16U) |
			  fields, adc->base + SARADC_CONV_CON);
	while (timeout_us--) {
		if (adc->ops->read32(adc->base + SARADC_END_INT_ST) &
		    SARADC_END_BIT) {
			complete = 1;
			break;
		}
		adc->ops->delay_us(1U);
	}
	if (!complete)
		return -2;
	value = adc->ops->read32(adc->base + SARADC_DATA(channel));
	adc->ops->write32(SARADC_END_BIT, adc->base + SARADC_END_INT_ST);
	adc->ops->write32(SARADC_END_BIT << 16U,
			  adc->base + SARADC_END_INT_EN);
	*sample = value & 0xfffU;
	return 0;
}
