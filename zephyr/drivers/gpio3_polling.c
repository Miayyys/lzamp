/* SPDX-License-Identifier: MIT */
#include "gpio3_polling.h"

#include <stddef.h>

#define RK3588_GPIO_DR_HIGH_OFFSET  0x04U
#define RK3588_GPIO_DDR_HIGH_OFFSET 0x0cU
#define RK3588_GPIO_EXT_PORT_OFFSET 0x70U

#define RK3588_GPIO3_D_IOMUX_LOW_OFFSET  0x8078U
#define RK3588_GPIO3_D_IOMUX_HIGH_OFFSET 0x807cU

static int line_is_owned(uint32_t line)
{
	return line == LZAMP_GPIO3_PD1 || line == LZAMP_GPIO3_PD4 ||
	       line == LZAMP_GPIO3_PD5;
}

static void write_masked_bit(const struct lzamp_gpio3 *device,
			     uintptr_t offset, uint32_t line, int value)
{
	uint32_t bit = line - 16U;
	uint32_t word = (1U << (bit + 16U));

	if (value)
		word |= 1U << bit;
	device->ops->write32(word, device->gpio_base + offset);
}

static void select_gpio_function(const struct lzamp_gpio3 *device,
				 uint32_t line)
{
	uintptr_t offset;
	uint32_t bit;

	if (line < LZAMP_GPIO3_PD4) {
		offset = RK3588_GPIO3_D_IOMUX_LOW_OFFSET;
		bit = (line - 24U) * 4U;
	} else {
		offset = RK3588_GPIO3_D_IOMUX_HIGH_OFFSET;
		bit = (line - 28U) * 4U;
	}

	/* RK3588 IOC: upper half is the nibble write mask, mux value 0 is GPIO. */
	device->ops->write32(0xfU << (bit + 16U), device->ioc_base + offset);
}

int lzamp_gpio3_init(struct lzamp_gpio3 *device, uintptr_t gpio_base,
		     uintptr_t ioc_base,
		     const struct lzamp_gpio3_mmio_ops *ops)
{
	if (!device || !ops || !ops->read32 || !ops->write32)
		return LZAMP_GPIO3_INVALID;

	device->gpio_base = gpio_base;
	device->ioc_base = ioc_base;
	device->ops = ops;
	device->output_mask = 0U;
	return LZAMP_GPIO3_OK;
}

int lzamp_gpio3_configure_input(struct lzamp_gpio3 *device, uint32_t line)
{
	if (!device || !device->ops || !line_is_owned(line))
		return LZAMP_GPIO3_INVALID;

	select_gpio_function(device, line);
	write_masked_bit(device, RK3588_GPIO_DDR_HIGH_OFFSET, line, 0);
	device->output_mask &= ~(1U << line);
	return LZAMP_GPIO3_OK;
}

int lzamp_gpio3_configure_output(struct lzamp_gpio3 *device, uint32_t line,
				 int initial_value)
{
	if (!device || !device->ops || !line_is_owned(line))
		return LZAMP_GPIO3_INVALID;

	select_gpio_function(device, line);
	/* Publish the initial value before enabling output to avoid a glitch. */
	write_masked_bit(device, RK3588_GPIO_DR_HIGH_OFFSET, line,
			 initial_value != 0);
	write_masked_bit(device, RK3588_GPIO_DDR_HIGH_OFFSET, line, 1);
	device->output_mask |= 1U << line;
	return LZAMP_GPIO3_OK;
}

int lzamp_gpio3_set(struct lzamp_gpio3 *device, uint32_t line, int value)
{
	if (!device || !device->ops || !line_is_owned(line))
		return LZAMP_GPIO3_INVALID;
	if (!(device->output_mask & (1U << line)))
		return LZAMP_GPIO3_NOT_OUTPUT;

	write_masked_bit(device, RK3588_GPIO_DR_HIGH_OFFSET, line, value != 0);
	return LZAMP_GPIO3_OK;
}

int lzamp_gpio3_get(const struct lzamp_gpio3 *device, uint32_t line,
		    int *value)
{
	uint32_t port;

	if (!device || !device->ops || !value || !line_is_owned(line))
		return LZAMP_GPIO3_INVALID;

	port = device->ops->read32(device->gpio_base +
				  RK3588_GPIO_EXT_PORT_OFFSET);
	*value = (port >> line) & 1U;
	return LZAMP_GPIO3_OK;
}
