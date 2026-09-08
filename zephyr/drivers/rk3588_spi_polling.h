/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_SPI_POLLING_H
#define LZAMP_RK3588_SPI_POLLING_H

#include <stddef.h>
#include "rk3588_mmio.h"

struct lzamp_rk3588_spi {
	uintptr_t base;
	const struct lzamp_mmio_ops *ops;
	uint32_t ctrlr0;
	uint32_t baud_div;
};

int lzamp_rk3588_spi_init(struct lzamp_rk3588_spi *spi, uintptr_t base,
			  const struct lzamp_mmio_ops *ops,
			  uint32_t input_hz, uint32_t frequency_hz,
			  uint32_t mode);
int lzamp_rk3588_spi_set_mode(struct lzamp_rk3588_spi *spi, uint32_t mode);
int lzamp_rk3588_spi_transfer(struct lzamp_rk3588_spi *spi, uint32_t chip_select,
			      const uint8_t *tx, uint8_t *rx, size_t length,
			      uint32_t timeout_us);

#endif
