/* SPDX-License-Identifier: MIT */
#include "rk3588_reset.h"

#include <stddef.h>

#define SOFTRST_CON(n) (0xa00U + (n) * 4U)

struct reset_line {
	uint8_t reg;
	uint8_t bit;
};

static void reset_line_write(const struct lzamp_rk3588_reset_controller *reset,
			     struct reset_line line, int asserted)
{
	uint32_t mask = 1U << line.bit;
	reset->ops->write32((mask << 16U) | (asserted ? mask : 0U),
			    reset->cru_base + SOFTRST_CON(line.reg));
}

int lzamp_rk3588_reset_init(struct lzamp_rk3588_reset_controller *reset,
			    uintptr_t cru_base,
			    const struct lzamp_mmio_ops *ops)
{
	if (!reset || !ops || !ops->write32 || !ops->delay_us)
		return -1;
	reset->cru_base = cru_base;
	reset->ops = ops;
	return 0;
}

int lzamp_rk3588_reset_pulse(struct lzamp_rk3588_reset_controller *reset,
			     enum lzamp_rk3588_reset peripheral)
{
	struct reset_line lines[2];
	uint32_t count = 2U;
	uint32_t index;

	if (!reset || !reset->ops)
		return -1;
	switch (peripheral) {
	case LZAMP_RESET_UART5:
		lines[0] = (struct reset_line){ 12U, 6U };
		lines[1] = (struct reset_line){ 13U, 9U };
		break;
	case LZAMP_RESET_UART7:
		lines[0] = (struct reset_line){ 12U, 8U };
		lines[1] = (struct reset_line){ 13U, 15U };
		break;
	case LZAMP_RESET_I2C7:
		lines[0] = (struct reset_line){ 10U, 14U };
		lines[1] = (struct reset_line){ 11U, 6U };
		break;
	case LZAMP_RESET_SPI0:
		lines[0] = (struct reset_line){ 14U, 6U };
		lines[1] = (struct reset_line){ 14U, 11U };
		break;
	case LZAMP_RESET_PWM1:
		lines[0] = (struct reset_line){ 15U, 3U };
		lines[1] = (struct reset_line){ 15U, 4U };
		break;
	case LZAMP_RESET_SARADC:
		lines[0] = (struct reset_line){ 11U, 14U };
		count = 1U;
		break;
	default:
		return -1;
	}
	for (index = 0; index < count; index++)
		reset_line_write(reset, lines[index], 1);
	reset->ops->delay_us(peripheral == LZAMP_RESET_SARADC ? 10U : 2U);
	for (index = 0; index < count; index++)
		reset_line_write(reset, lines[index], 0);
	reset->ops->delay_us(2U);
	return 0;
}
