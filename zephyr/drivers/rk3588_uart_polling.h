/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_UART_POLLING_H
#define LZAMP_RK3588_UART_POLLING_H

#include <stddef.h>
#include "rk3588_mmio.h"

struct lzamp_rk3588_uart {
	uintptr_t base;
	const struct lzamp_mmio_ops *ops;
};

int lzamp_rk3588_uart_init(struct lzamp_rk3588_uart *uart, uintptr_t base,
			   const struct lzamp_mmio_ops *ops,
			   uint32_t input_hz, uint32_t baud);
int lzamp_rk3588_uart_write(struct lzamp_rk3588_uart *uart,
			    const uint8_t *data, size_t length,
			    uint32_t timeout_us);
int lzamp_rk3588_uart_read(struct lzamp_rk3588_uart *uart, uint8_t *data,
			   size_t capacity, size_t *received,
			   uint32_t timeout_us);

#endif
