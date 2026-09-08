/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_GPIO3_POLLING_H
#define LZAMP_GPIO3_POLLING_H

#include <stdint.h>

#define LZAMP_GPIO3_PD1 25U
#define LZAMP_GPIO3_PD4 28U
#define LZAMP_GPIO3_PD5 29U

struct lzamp_gpio3_mmio_ops {
	uint32_t (*read32)(uintptr_t address);
	void (*write32)(uint32_t value, uintptr_t address);
};

struct lzamp_gpio3 {
	uintptr_t gpio_base;
	uintptr_t ioc_base;
	const struct lzamp_gpio3_mmio_ops *ops;
	uint32_t output_mask;
};

enum lzamp_gpio3_result {
	LZAMP_GPIO3_OK = 0,
	LZAMP_GPIO3_INVALID = -1,
	LZAMP_GPIO3_NOT_OUTPUT = -2,
};

int lzamp_gpio3_init(struct lzamp_gpio3 *device, uintptr_t gpio_base,
		     uintptr_t ioc_base,
		     const struct lzamp_gpio3_mmio_ops *ops);
int lzamp_gpio3_configure_input(struct lzamp_gpio3 *device, uint32_t line);
int lzamp_gpio3_configure_output(struct lzamp_gpio3 *device, uint32_t line,
				 int initial_value);
int lzamp_gpio3_set(struct lzamp_gpio3 *device, uint32_t line, int value);
int lzamp_gpio3_get(const struct lzamp_gpio3 *device, uint32_t line,
		    int *value);

#endif
