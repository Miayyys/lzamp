/* SPDX-License-Identifier: MIT */
#include "rk3588_peripheral_clock.h"

#include <stddef.h>

#define RK3588_CLKSEL_CON(n)  (0x300U + ((n) * 4U))
#define RK3588_CLKGATE_CON(n) (0x800U + ((n) * 4U))

#define UART5_OUTPUT_MUX_CON RK3588_CLKSEL_CON(51)
#define UART7_OUTPUT_MUX_CON RK3588_CLKSEL_CON(55)
#define UART_OUTPUT_MUX_SHIFT 0U
#define UART_OUTPUT_MUX_MASK  0x3U
#define UART_OUTPUT_XIN24M    0x2U

#define I2C7_PARENT_MUX_CON   RK3588_CLKSEL_CON(38)
#define I2C7_PARENT_MUX_SHIFT 12U
#define I2C7_PARENT_MUX_MASK  0x1U
#define I2C7_PARENT_100M      0x1U

#define SHARED_PERIPH_MUX_CON RK3588_CLKSEL_CON(59)
#define SPI0_PARENT_MUX_SHIFT 2U
#define SPI0_PARENT_MUX_MASK  0x3U
#define SPI0_PARENT_XIN24M    0x2U
#define PWM1_PARENT_MUX_SHIFT 12U
#define PWM1_PARENT_MUX_MASK  0x3U
#define PWM1_PARENT_XIN24M    0x2U

#define SARADC_CLOCK_CON       RK3588_CLKSEL_CON(40)
#define SARADC_PARENT_SHIFT    14U
#define SARADC_PARENT_MASK     0x1U
#define SARADC_PARENT_XIN24M   0x1U
#define SARADC_DIVIDER_SHIFT   6U
#define SARADC_DIVIDER_MASK    0xffU
#define SARADC_DIVIDER_1MHZ    23U

#define TOP_PCLK_GATE_CON RK3588_CLKGATE_CON(10)
#define PERIPH_GATE_CON   RK3588_CLKGATE_CON(11)
#define UART_PCLK_GATE_CON RK3588_CLKGATE_CON(12)
#define UART_SCLK_GATE_CON RK3588_CLKGATE_CON(13)
#define SPI_GATE_CON       RK3588_CLKGATE_CON(14)
#define PWM_GATE_CON       RK3588_CLKGATE_CON(15)

#define PCLK_I2C7_GATE_BIT  14U
#define CLK_I2C7_GATE_BIT   6U
#define PCLK_UART5_GATE_BIT 6U
#define PCLK_UART7_GATE_BIT 8U
#define SCLK_UART5_GATE_BIT 9U
#define SCLK_UART7_GATE_BIT 15U
#define PCLK_SPI0_GATE_BIT  6U
#define CLK_SPI0_GATE_BIT   11U
#define PCLK_PWM1_GATE_BIT  3U
#define CLK_PWM1_GATE_BIT   4U
#define CAP_PWM1_GATE_BIT   5U
#define PCLK_SARADC_GATE_BIT 14U
#define CLK_SARADC_GATE_BIT  15U

static void hiword_update(struct lzamp_rk3588_cru *cru, uint32_t offset,
			  uint32_t mask, uint32_t shift, uint32_t value)
{
	uint32_t shifted_mask = mask << shift;
	uint32_t word = (shifted_mask << 16U) |
			((value & mask) << shift);

	cru->ops->write32(word, cru->base + offset);
}

static void gate_set(struct lzamp_rk3588_cru *cru, uint32_t offset,
		     uint32_t bit, int enable)
{
	/* RK3588 gate bits are 1=disabled; the upper half is the write mask. */
	hiword_update(cru, offset, 1U, bit, enable ? 0U : 1U);
}

int lzamp_rk3588_cru_init(struct lzamp_rk3588_cru *cru, uintptr_t base,
			  const struct lzamp_rk3588_cru_ops *ops)
{
	if (!cru || !ops || !ops->write32)
		return LZAMP_RK3588_CLOCK_INVALID;

	cru->base = base;
	cru->ops = ops;
	return LZAMP_RK3588_CLOCK_OK;
}

static void uart_enable(struct lzamp_rk3588_cru *cru, uint32_t mux_con,
			uint32_t pclk_bit, uint32_t sclk_bit)
{
	/* Stop the leaf clock before switching its instance-local output mux. */
	gate_set(cru, UART_SCLK_GATE_CON, sclk_bit, 0);
	hiword_update(cru, mux_con, UART_OUTPUT_MUX_MASK,
		      UART_OUTPUT_MUX_SHIFT, UART_OUTPUT_XIN24M);
	gate_set(cru, UART_PCLK_GATE_CON, pclk_bit, 1);
	gate_set(cru, UART_SCLK_GATE_CON, sclk_bit, 1);
}

int lzamp_rk3588_peripheral_clock_enable(
	struct lzamp_rk3588_cru *cru,
	enum lzamp_rk3588_peripheral_clock peripheral)
{
	if (!cru || !cru->ops || !cru->ops->write32)
		return LZAMP_RK3588_CLOCK_INVALID;

	switch (peripheral) {
	case LZAMP_RK3588_CLOCK_UART5:
		uart_enable(cru, UART5_OUTPUT_MUX_CON,
			    PCLK_UART5_GATE_BIT, SCLK_UART5_GATE_BIT);
		break;
	case LZAMP_RK3588_CLOCK_UART7:
		uart_enable(cru, UART7_OUTPUT_MUX_CON,
			    PCLK_UART7_GATE_BIT, SCLK_UART7_GATE_BIT);
		break;
	case LZAMP_RK3588_CLOCK_I2C7:
		gate_set(cru, PERIPH_GATE_CON, CLK_I2C7_GATE_BIT, 0);
		hiword_update(cru, I2C7_PARENT_MUX_CON, I2C7_PARENT_MUX_MASK,
			      I2C7_PARENT_MUX_SHIFT, I2C7_PARENT_100M);
		gate_set(cru, TOP_PCLK_GATE_CON, PCLK_I2C7_GATE_BIT, 1);
		gate_set(cru, PERIPH_GATE_CON, CLK_I2C7_GATE_BIT, 1);
		break;
	case LZAMP_RK3588_CLOCK_SPI0:
		gate_set(cru, SPI_GATE_CON, CLK_SPI0_GATE_BIT, 0);
		hiword_update(cru, SHARED_PERIPH_MUX_CON,
			      SPI0_PARENT_MUX_MASK, SPI0_PARENT_MUX_SHIFT,
			      SPI0_PARENT_XIN24M);
		gate_set(cru, SPI_GATE_CON, PCLK_SPI0_GATE_BIT, 1);
		gate_set(cru, SPI_GATE_CON, CLK_SPI0_GATE_BIT, 1);
		break;
	case LZAMP_RK3588_CLOCK_PWM1:
		gate_set(cru, PWM_GATE_CON, CLK_PWM1_GATE_BIT, 0);
		gate_set(cru, PWM_GATE_CON, CAP_PWM1_GATE_BIT, 0);
		hiword_update(cru, SHARED_PERIPH_MUX_CON,
			      PWM1_PARENT_MUX_MASK, PWM1_PARENT_MUX_SHIFT,
			      PWM1_PARENT_XIN24M);
		gate_set(cru, PWM_GATE_CON, PCLK_PWM1_GATE_BIT, 1);
		gate_set(cru, PWM_GATE_CON, CLK_PWM1_GATE_BIT, 1);
		gate_set(cru, PWM_GATE_CON, CAP_PWM1_GATE_BIT, 1);
		break;
	case LZAMP_RK3588_CLOCK_SARADC:
		gate_set(cru, PERIPH_GATE_CON, CLK_SARADC_GATE_BIT, 0);
		hiword_update(cru, SARADC_CLOCK_CON, SARADC_PARENT_MASK,
			      SARADC_PARENT_SHIFT, SARADC_PARENT_XIN24M);
		hiword_update(cru, SARADC_CLOCK_CON, SARADC_DIVIDER_MASK,
			      SARADC_DIVIDER_SHIFT, SARADC_DIVIDER_1MHZ);
		gate_set(cru, PERIPH_GATE_CON, PCLK_SARADC_GATE_BIT, 1);
		gate_set(cru, PERIPH_GATE_CON, CLK_SARADC_GATE_BIT, 1);
		break;
	default:
		return LZAMP_RK3588_CLOCK_INVALID;
	}

	return LZAMP_RK3588_CLOCK_OK;
}

int lzamp_rk3588_peripheral_clock_disable(
	struct lzamp_rk3588_cru *cru,
	enum lzamp_rk3588_peripheral_clock peripheral)
{
	if (!cru || !cru->ops || !cru->ops->write32)
		return LZAMP_RK3588_CLOCK_INVALID;

	switch (peripheral) {
	case LZAMP_RK3588_CLOCK_UART5:
		gate_set(cru, UART_SCLK_GATE_CON, SCLK_UART5_GATE_BIT, 0);
		gate_set(cru, UART_PCLK_GATE_CON, PCLK_UART5_GATE_BIT, 0);
		break;
	case LZAMP_RK3588_CLOCK_UART7:
		gate_set(cru, UART_SCLK_GATE_CON, SCLK_UART7_GATE_BIT, 0);
		gate_set(cru, UART_PCLK_GATE_CON, PCLK_UART7_GATE_BIT, 0);
		break;
	case LZAMP_RK3588_CLOCK_I2C7:
		gate_set(cru, PERIPH_GATE_CON, CLK_I2C7_GATE_BIT, 0);
		gate_set(cru, TOP_PCLK_GATE_CON, PCLK_I2C7_GATE_BIT, 0);
		break;
	case LZAMP_RK3588_CLOCK_SPI0:
		gate_set(cru, SPI_GATE_CON, CLK_SPI0_GATE_BIT, 0);
		gate_set(cru, SPI_GATE_CON, PCLK_SPI0_GATE_BIT, 0);
		break;
	case LZAMP_RK3588_CLOCK_PWM1:
		gate_set(cru, PWM_GATE_CON, CAP_PWM1_GATE_BIT, 0);
		gate_set(cru, PWM_GATE_CON, CLK_PWM1_GATE_BIT, 0);
		gate_set(cru, PWM_GATE_CON, PCLK_PWM1_GATE_BIT, 0);
		break;
	case LZAMP_RK3588_CLOCK_SARADC:
		gate_set(cru, PERIPH_GATE_CON, CLK_SARADC_GATE_BIT, 0);
		gate_set(cru, PERIPH_GATE_CON, PCLK_SARADC_GATE_BIT, 0);
		break;
	default:
		return LZAMP_RK3588_CLOCK_INVALID;
	}

	return LZAMP_RK3588_CLOCK_OK;
}
