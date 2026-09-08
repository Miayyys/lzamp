/* SPDX-License-Identifier: MIT */
#include "rk3588_pinctrl.h"

#include <stddef.h>

#define GPIO3_C_IOMUX_LOW  0x8070U
#define GPIO3_C_IOMUX_HIGH 0x8074U
#define GPIO3_D_IOMUX_LOW  0x8078U
#define GPIO3_D_IOMUX_HIGH 0x807cU
#define GPIO0_D_IOMUX_PMU2 0x400cU
#define GPIO0_D_IOMUX_BUS  0x8018U

static void mux_nibble(struct lzamp_rk3588_pinctrl *pinctrl,
		       uint32_t offset, uint32_t nibble, uint32_t function)
{
	uint32_t shift = nibble * 4U;
	uint32_t mask = 0xfU << shift;

	pinctrl->ops->write32((mask << 16U) | (function << shift),
			       pinctrl->ioc_base + offset);
}

int lzamp_rk3588_pinctrl_init(struct lzamp_rk3588_pinctrl *pinctrl,
			      uintptr_t ioc_base,
			      const struct lzamp_mmio_ops *ops)
{
	if (!pinctrl || !ops || !ops->write32)
		return -1;
	pinctrl->ioc_base = ioc_base;
	pinctrl->ops = ops;
	return 0;
}

int lzamp_rk3588_pinctrl_uart5(struct lzamp_rk3588_pinctrl *pinctrl)
{
	if (!pinctrl || !pinctrl->ops)
		return -1;
	/* UART5 M1: GPIO3_C4 TX, GPIO3_C5 RX, function 10. */
	mux_nibble(pinctrl, GPIO3_C_IOMUX_HIGH, 0U, 10U);
	mux_nibble(pinctrl, GPIO3_C_IOMUX_HIGH, 1U, 10U);
	return 0;
}

int lzamp_rk3588_pinctrl_uart7(struct lzamp_rk3588_pinctrl *pinctrl)
{
	if (!pinctrl || !pinctrl->ops)
		return -1;
	/* UART7 M1: GPIO3_C0 TX, GPIO3_C1 RX, function 10. */
	mux_nibble(pinctrl, GPIO3_C_IOMUX_LOW, 0U, 10U);
	mux_nibble(pinctrl, GPIO3_C_IOMUX_LOW, 1U, 10U);
	return 0;
}

int lzamp_rk3588_pinctrl_profile(struct lzamp_rk3588_pinctrl *pinctrl,
				 enum lzamp_rk3588_pin_profile profile)
{
	if (!pinctrl || !pinctrl->ops)
		return -1;
	if (profile == LZAMP_PIN_PROFILE_I2C7_GPIO) {
		/* I2C7 M2: GPIO3_D2 SDA and D3 SCL, function 9. */
		mux_nibble(pinctrl, GPIO3_D_IOMUX_LOW, 2U, 9U);
		mux_nibble(pinctrl, GPIO3_D_IOMUX_LOW, 3U, 9U);
		return 0;
	}
	if (profile == LZAMP_PIN_PROFILE_SPI0) {
		/* SPI0 M3: D1 MISO, D2 MOSI, D3 CLK, D4/D5 CS0/CS1. */
		mux_nibble(pinctrl, GPIO3_D_IOMUX_LOW, 1U, 8U);
		mux_nibble(pinctrl, GPIO3_D_IOMUX_LOW, 2U, 8U);
		mux_nibble(pinctrl, GPIO3_D_IOMUX_LOW, 3U, 8U);
		mux_nibble(pinctrl, GPIO3_D_IOMUX_HIGH, 0U, 8U);
		mux_nibble(pinctrl, GPIO3_D_IOMUX_HIGH, 1U, 8U);
		return 0;
	}
	return -1;
}

int lzamp_rk3588_pinctrl_pwm7(struct lzamp_rk3588_pinctrl *pinctrl)
{
	if (!pinctrl || !pinctrl->ops)
		return -1;
	/* GPIO0_D0 high mux: PMU2 selects the BUS stage, BUS selects PWM7 M0. */
	mux_nibble(pinctrl, GPIO0_D_IOMUX_PMU2, 0U, 8U);
	mux_nibble(pinctrl, GPIO0_D_IOMUX_BUS, 0U, 11U);
	return 0;
}
