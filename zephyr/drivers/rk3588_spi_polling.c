/* SPDX-License-Identifier: MIT */
#include "rk3588_spi_polling.h"

#include <stddef.h>

#define SPI_CTRLR0 0x000U
#define SPI_CTRLR1 0x004U
#define SPI_SSIENR 0x008U
#define SPI_SER    0x00cU
#define SPI_BAUDR  0x010U
#define SPI_TXFTLR 0x014U
#define SPI_RXFTLR 0x018U
#define SPI_SR     0x024U
#define SPI_IMR    0x02cU
#define SPI_ICR    0x038U
#define SPI_DMACR  0x03cU
#define SPI_TXDR   0x400U
#define SPI_RXDR   0x800U
#define SPI_SR_BUSY     (1U << 0)
#define SPI_SR_TF_FULL  (1U << 1)
#define SPI_SR_RF_EMPTY (1U << 3)
#define SPI_CR0_DFS_8BIT (1U << 0)
#define SPI_CR0_CPHA     (1U << 6)
#define SPI_CR0_CPOL     (1U << 7)
#define SPI_CR0_BHT_8BIT (1U << 13)

static int wait_status(struct lzamp_rk3588_spi *spi, uint32_t mask,
		       int want_set, uint32_t timeout_us)
{
	while (timeout_us--) {
		int set = (spi->ops->read32(spi->base + SPI_SR) & mask) != 0U;
		if (set == want_set)
			return 0;
		spi->ops->delay_us(1U);
	}
	return -2;
}

int lzamp_rk3588_spi_init(struct lzamp_rk3588_spi *spi, uintptr_t base,
			  const struct lzamp_mmio_ops *ops,
			  uint32_t input_hz, uint32_t frequency_hz,
			  uint32_t mode)
{
	uint32_t divider;

	if (!spi || !ops || !ops->read32 || !ops->write32 || !ops->delay_us ||
	    !input_hz || !frequency_hz || mode > 3U)
		return -1;
	divider = (input_hz + frequency_hz - 1U) / frequency_hz;
	if (divider < 2U)
		divider = 2U;
	if (divider & 1U)
		divider++;
	if (divider > 65534U)
		return -1;
	spi->base = base;
	spi->ops = ops;
	spi->baud_div = divider;
	spi->ctrlr0 = SPI_CR0_DFS_8BIT | SPI_CR0_BHT_8BIT;
	ops->write32(0U, base + SPI_SSIENR);
	ops->write32(0U, base + SPI_IMR);
	ops->write32(0U, base + SPI_DMACR);
	ops->write32(1U, base + SPI_ICR);
	return lzamp_rk3588_spi_set_mode(spi, mode);
}

int lzamp_rk3588_spi_set_mode(struct lzamp_rk3588_spi *spi, uint32_t mode)
{
	if (!spi || !spi->ops || mode > 3U)
		return -1;
	spi->ops->write32(0U, spi->base + SPI_SSIENR);
	spi->ctrlr0 &= ~(SPI_CR0_CPHA | SPI_CR0_CPOL);
	if (mode & 1U)
		spi->ctrlr0 |= SPI_CR0_CPHA;
	if (mode & 2U)
		spi->ctrlr0 |= SPI_CR0_CPOL;
	return 0;
}

int lzamp_rk3588_spi_transfer(struct lzamp_rk3588_spi *spi, uint32_t chip_select,
			      const uint8_t *tx, uint8_t *rx, size_t length,
			      uint32_t timeout_us)
{
	size_t index;

	if (!spi || !spi->ops || chip_select > 1U || !length ||
	    (!tx && !rx) || length > 65535U)
		return -1;
	spi->ops->write32(0U, spi->base + SPI_SSIENR);
	spi->ops->write32(spi->ctrlr0, spi->base + SPI_CTRLR0);
	spi->ops->write32((uint32_t)length - 1U, spi->base + SPI_CTRLR1);
	spi->ops->write32(spi->baud_div, spi->base + SPI_BAUDR);
	spi->ops->write32(0U, spi->base + SPI_TXFTLR);
	spi->ops->write32(0U, spi->base + SPI_RXFTLR);
	spi->ops->write32(1U << chip_select, spi->base + SPI_SER);
	spi->ops->write32(1U, spi->base + SPI_SSIENR);
	for (index = 0; index < length; index++) {
		if (wait_status(spi, SPI_SR_TF_FULL, 0, timeout_us))
			goto timeout;
		spi->ops->write32(tx ? tx[index] : 0U, spi->base + SPI_TXDR);
		if (wait_status(spi, SPI_SR_RF_EMPTY, 0, timeout_us))
			goto timeout;
		if (rx)
			rx[index] = spi->ops->read32(spi->base + SPI_RXDR);
		else
			(void)spi->ops->read32(spi->base + SPI_RXDR);
	}
	if (wait_status(spi, SPI_SR_BUSY, 0, timeout_us))
		goto timeout;
	spi->ops->write32(0U, spi->base + SPI_SSIENR);
	spi->ops->write32(0U, spi->base + SPI_SER);
	return 0;
timeout:
	spi->ops->write32(0U, spi->base + SPI_SSIENR);
	spi->ops->write32(0U, spi->base + SPI_SER);
	return -2;
}
