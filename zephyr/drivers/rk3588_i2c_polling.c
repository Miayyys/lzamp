/* SPDX-License-Identifier: MIT */
#include "rk3588_i2c_polling.h"

#include <stddef.h>

#define I2C_CON      0x000U
#define I2C_CLKDIV   0x004U
#define I2C_MRXADDR  0x008U
#define I2C_MRXRADDR 0x00cU
#define I2C_MTXCNT   0x010U
#define I2C_MRXCNT   0x014U
#define I2C_IEN      0x018U
#define I2C_IPD      0x01cU
#define I2C_TXBUF    0x100U
#define I2C_RXBUF    0x200U
#define CON_EN       (1U << 0)
#define CON_MODE_TX  (0U << 1)
#define CON_MODE_REGISTER_TX (1U << 1)
#define CON_START    (1U << 3)
#define CON_STOP     (1U << 4)
#define CON_LASTACK  (1U << 5)
#define CON_ACTACK   (1U << 6)
#define INT_MBTF     (1U << 2)
#define INT_MBRF     (1U << 3)
#define INT_STOP     (1U << 5)
#define INT_NAK      (1U << 6)
#define INT_ALL      0xfffU
#define ADDR_VALID(n) (1U << (24U + (n)))

static int wait_ipd(struct lzamp_rk3588_i2c *i2c, uint32_t desired,
		    uint32_t timeout_us, uint32_t *ipd)
{
	while (timeout_us) {
		*ipd = i2c->ops->read32(i2c->base + I2C_IPD);
		if (*ipd & INT_NAK) {
			i2c->ops->write32(*ipd & INT_ALL, i2c->base + I2C_IPD);
			return -3;
		}
		if (*ipd & desired)
			return 0;
		i2c->ops->delay_us(5U);
		timeout_us = timeout_us > 5U ? timeout_us - 5U : 0U;
	}
	return -2;
}

static int stop_bus(struct lzamp_rk3588_i2c *i2c, uint32_t timeout_us)
{
	uint32_t con, ipd;

	i2c->ops->write32(INT_STOP | INT_NAK, i2c->base + I2C_IEN);
	con = i2c->ops->read32(i2c->base + I2C_CON);
	con = (con | CON_STOP) & ~CON_START;
	i2c->ops->write32(con, i2c->base + I2C_CON);
	if (wait_ipd(i2c, INT_STOP, timeout_us, &ipd)) {
		i2c->ops->write32(0U, i2c->base + I2C_IEN);
		i2c->ops->write32(0U, i2c->base + I2C_CON);
		return -2;
	}
	i2c->ops->write32(ipd & INT_ALL, i2c->base + I2C_IPD);
	i2c->ops->write32(0U, i2c->base + I2C_IEN);
	i2c->ops->write32(0U, i2c->base + I2C_CON);
	return 0;
}

static void fill_tx(struct lzamp_rk3588_i2c *i2c, uint8_t address,
		    const uint8_t *data, size_t length)
{
	uint32_t words[8] = { 0 };
	size_t index;

	words[0] = (uint32_t)address << 1U;
	for (index = 0; index < length; index++) {
		size_t pos = index + 1U;
		words[pos / 4U] |= (uint32_t)data[index] << ((pos % 4U) * 8U);
	}
	for (index = 0; index < 8U && index * 4U < length + 1U; index++)
		i2c->ops->write32(words[index], i2c->base + I2C_TXBUF + index * 4U);
}

int lzamp_rk3588_i2c_init(struct lzamp_rk3588_i2c *i2c, uintptr_t base,
			  const struct lzamp_mmio_ops *ops,
			  uint32_t clock_hz, uint32_t bus_hz)
{
	uint32_t divider;

	if (!i2c || !ops || !ops->read32 || !ops->write32 || !ops->delay_us ||
	    !clock_hz || !bus_hz || bus_hz > 400000U)
		return -1;
	/* RK3x SCL low/high periods are (divider + 1) * 8 input clocks. */
	divider = (clock_hz + (16U * bus_hz) - 1U) / (16U * bus_hz);
	if (!divider || divider > 65536U)
		return -1;
	divider--;
	i2c->base = base;
	i2c->ops = ops;
	i2c->clock_hz = clock_hz;
	i2c->bus_hz = bus_hz;
	ops->write32(0U, base + I2C_CON);
	ops->write32(0U, base + I2C_IEN);
	ops->write32(INT_ALL, base + I2C_IPD);
	ops->write32((divider << 16U) | divider, base + I2C_CLKDIV);
	return 0;
}

int lzamp_rk3588_i2c_write(struct lzamp_rk3588_i2c *i2c, uint8_t address,
			   const uint8_t *data, size_t length,
			   uint32_t timeout_us)
{
	uint32_t ipd;
	int ret;

	if (!i2c || !i2c->ops || address > 0x7fU || !data ||
	    !length || length > 31U)
		return -1;
	i2c->ops->write32(INT_ALL, i2c->base + I2C_IPD);
	fill_tx(i2c, address, data, length);
	i2c->ops->write32(INT_MBTF | INT_NAK, i2c->base + I2C_IEN);
	i2c->ops->write32(CON_EN | CON_MODE_TX | CON_START | CON_ACTACK,
			  i2c->base + I2C_CON);
	i2c->ops->write32((uint32_t)length + 1U, i2c->base + I2C_MTXCNT);
	ret = wait_ipd(i2c, INT_MBTF, timeout_us, &ipd);
	if (!ret)
		i2c->ops->write32(ipd & INT_ALL, i2c->base + I2C_IPD);
	if (stop_bus(i2c, timeout_us) && !ret)
		ret = -2;
	return ret;
}

static int read_common(struct lzamp_rk3588_i2c *i2c, uint8_t address,
		       const uint8_t *prefix, size_t prefix_length,
		       uint8_t *data, size_t length, uint32_t timeout_us)
{
	uint32_t reg_address = 0U, ipd, word = 0U;
	size_t index;
	int ret;

	if (!i2c || !i2c->ops || address > 0x7fU || !data || !length ||
	    length > 32U || prefix_length > 3U || (prefix_length && !prefix))
		return -1;
	for (index = 0; index < prefix_length; index++) {
		reg_address |= (uint32_t)prefix[index] << (index * 8U);
		reg_address |= ADDR_VALID(index);
	}
	i2c->ops->write32(INT_ALL, i2c->base + I2C_IPD);
	i2c->ops->write32(((uint32_t)address << 1U) | 1U | ADDR_VALID(0),
			  i2c->base + I2C_MRXADDR);
	i2c->ops->write32(reg_address, i2c->base + I2C_MRXRADDR);
	i2c->ops->write32(INT_MBRF | INT_NAK, i2c->base + I2C_IEN);
	i2c->ops->write32(CON_EN | CON_MODE_REGISTER_TX | CON_START |
			  CON_LASTACK | CON_ACTACK, i2c->base + I2C_CON);
	i2c->ops->write32((uint32_t)length, i2c->base + I2C_MRXCNT);
	ret = wait_ipd(i2c, INT_MBRF, timeout_us, &ipd);
	if (!ret) {
		for (index = 0; index < length; index++) {
			if ((index % 4U) == 0U)
				word = i2c->ops->read32(i2c->base + I2C_RXBUF +
							(index / 4U) * 4U);
			data[index] = (word >> ((index % 4U) * 8U)) & 0xffU;
		}
		i2c->ops->write32(ipd & INT_ALL, i2c->base + I2C_IPD);
	}
	if (stop_bus(i2c, timeout_us) && !ret)
		ret = -2;
	return ret;
}

int lzamp_rk3588_i2c_read(struct lzamp_rk3588_i2c *i2c, uint8_t address,
			  uint8_t *data, size_t length, uint32_t timeout_us)
{
	return read_common(i2c, address, NULL, 0U, data, length, timeout_us);
}

int lzamp_rk3588_i2c_write_read(struct lzamp_rk3588_i2c *i2c, uint8_t address,
				const uint8_t *prefix, size_t prefix_length,
				uint8_t *data, size_t length,
				uint32_t timeout_us)
{
	if (!prefix_length)
		return -1;
	return read_common(i2c, address, prefix, prefix_length, data, length,
			   timeout_us);
}
