/* SPDX-License-Identifier: MIT */
#include "rk3588_uart_polling.h"

#include <stddef.h>

#define UART_RBR_THR_DLL 0x00U
#define UART_IER_DLM     0x04U
#define UART_IIR_FCR     0x08U
#define UART_LCR         0x0cU
#define UART_LSR         0x14U
#define UART_LCR_DLAB    (1U << 7)
#define UART_LCR_8N1     0x03U
#define UART_FCR_ENABLE_RESET 0x07U
#define UART_LSR_DR      (1U << 0)
#define UART_LSR_THRE    (1U << 5)

static int wait_lsr(struct lzamp_rk3588_uart *uart, uint32_t mask,
		    uint32_t timeout_us)
{
	while (timeout_us--) {
		if (uart->ops->read32(uart->base + UART_LSR) & mask)
			return 0;
		uart->ops->delay_us(1U);
	}
	return -2;
}

int lzamp_rk3588_uart_init(struct lzamp_rk3588_uart *uart, uintptr_t base,
			   const struct lzamp_mmio_ops *ops,
			   uint32_t input_hz, uint32_t baud)
{
	uint32_t divisor;

	if (!uart || !ops || !ops->read32 || !ops->write32 ||
	    !ops->delay_us || !input_hz || !baud)
		return -1;
	divisor = (input_hz + baud * 8U) / (baud * 16U);
	if (!divisor || divisor > 0xffffU)
		return -1;
	uart->base = base;
	uart->ops = ops;
	ops->write32(0U, base + UART_IER_DLM);
	ops->write32(UART_LCR_8N1 | UART_LCR_DLAB, base + UART_LCR);
	ops->write32(divisor & 0xffU, base + UART_RBR_THR_DLL);
	ops->write32(divisor >> 8U, base + UART_IER_DLM);
	ops->write32(UART_LCR_8N1, base + UART_LCR);
	ops->write32(UART_FCR_ENABLE_RESET, base + UART_IIR_FCR);
	return 0;
}

int lzamp_rk3588_uart_write(struct lzamp_rk3588_uart *uart,
			    const uint8_t *data, size_t length,
			    uint32_t timeout_us)
{
	size_t index;

	if (!uart || !uart->ops || (length && !data))
		return -1;
	for (index = 0; index < length; index++) {
		if (wait_lsr(uart, UART_LSR_THRE, timeout_us))
			return -2;
		uart->ops->write32(data[index], uart->base + UART_RBR_THR_DLL);
	}
	return 0;
}

int lzamp_rk3588_uart_read(struct lzamp_rk3588_uart *uart, uint8_t *data,
			   size_t capacity, size_t *received,
			   uint32_t timeout_us)
{
	if (!uart || !uart->ops || !data || !capacity || !received)
		return -1;
	*received = 0U;
	if (wait_lsr(uart, UART_LSR_DR, timeout_us))
		return -2;
	while (*received < capacity &&
	       (uart->ops->read32(uart->base + UART_LSR) & UART_LSR_DR)) {
		data[*received] = uart->ops->read32(uart->base + UART_RBR_THR_DLL);
		(*received)++;
	}
	return 0;
}
