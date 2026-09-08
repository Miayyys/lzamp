/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_I2C_POLLING_H
#define LZAMP_RK3588_I2C_POLLING_H

#include <stddef.h>
#include "rk3588_mmio.h"

struct lzamp_rk3588_i2c {
	uintptr_t base;
	const struct lzamp_mmio_ops *ops;
	uint32_t clock_hz;
	uint32_t bus_hz;
};

int lzamp_rk3588_i2c_init(struct lzamp_rk3588_i2c *i2c, uintptr_t base,
			  const struct lzamp_mmio_ops *ops,
			  uint32_t clock_hz, uint32_t bus_hz);
int lzamp_rk3588_i2c_write(struct lzamp_rk3588_i2c *i2c, uint8_t address,
			   const uint8_t *data, size_t length,
			   uint32_t timeout_us);
int lzamp_rk3588_i2c_read(struct lzamp_rk3588_i2c *i2c, uint8_t address,
			  uint8_t *data, size_t length, uint32_t timeout_us);
int lzamp_rk3588_i2c_write_read(struct lzamp_rk3588_i2c *i2c, uint8_t address,
				const uint8_t *prefix, size_t prefix_length,
				uint8_t *data, size_t length,
				uint32_t timeout_us);

#endif
