/* SPDX-License-Identifier: MIT */
#include "rk3588_peripheral_clock.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct write_record {
	uintptr_t address;
	uint32_t value;
};

static struct write_record writes[8];
static size_t write_count;

static void record_write(uint32_t value, uintptr_t address)
{
	assert(write_count < sizeof(writes) / sizeof(writes[0]));
	writes[write_count].address = address;
	writes[write_count].value = value;
	write_count++;
}

static const struct lzamp_rk3588_cru_ops ops = {
	.write32 = record_write,
};

static void reset_records(void)
{
	write_count = 0;
}

static void expect_write(size_t index, uintptr_t address, uint32_t value)
{
	assert(index < write_count);
	assert(writes[index].address == address);
	assert(writes[index].value == value);
}

int main(void)
{
	struct lzamp_rk3588_cru cru;
	const uintptr_t base = 0xfd7c0000U;

	assert(lzamp_rk3588_cru_init(&cru, base, &ops) == 0);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_UART5) == 0);
	assert(write_count == 4);
	expect_write(0, base + 0x834U, 0x02000200U);
	expect_write(1, base + 0x3ccU, 0x00030002U);
	expect_write(2, base + 0x830U, 0x00400000U);
	expect_write(3, base + 0x834U, 0x02000000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_UART7) == 0);
	assert(write_count == 4);
	expect_write(0, base + 0x834U, 0x80008000U);
	expect_write(1, base + 0x3dcU, 0x00030002U);
	expect_write(2, base + 0x830U, 0x01000000U);
	expect_write(3, base + 0x834U, 0x80000000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_I2C7) == 0);
	assert(write_count == 4);
	expect_write(0, base + 0x82cU, 0x00400040U);
	expect_write(1, base + 0x398U, 0x10001000U);
	expect_write(2, base + 0x828U, 0x40000000U);
	expect_write(3, base + 0x82cU, 0x00400000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_SPI0) == 0);
	assert(write_count == 4);
	expect_write(0, base + 0x838U, 0x08000800U);
	expect_write(1, base + 0x3ecU, 0x000c0008U);
	expect_write(2, base + 0x838U, 0x00400000U);
	expect_write(3, base + 0x838U, 0x08000000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_PWM1) == 0);
	assert(write_count == 6);
	expect_write(0, base + 0x83cU, 0x00100010U);
	expect_write(1, base + 0x83cU, 0x00200020U);
	expect_write(2, base + 0x3ecU, 0x30002000U);
	expect_write(3, base + 0x83cU, 0x00080000U);
	expect_write(4, base + 0x83cU, 0x00100000U);
	expect_write(5, base + 0x83cU, 0x00200000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_enable(
		       &cru, LZAMP_RK3588_CLOCK_SARADC) == 0);
	assert(write_count == 5);
	expect_write(0, base + 0x82cU, 0x80008000U);
	expect_write(1, base + 0x3a0U, 0x40004000U);
	expect_write(2, base + 0x3a0U, 0x3fc005c0U);
	expect_write(3, base + 0x82cU, 0x40000000U);
	expect_write(4, base + 0x82cU, 0x80000000U);

	reset_records();
	assert(lzamp_rk3588_peripheral_clock_disable(
		       &cru, LZAMP_RK3588_CLOCK_UART5) == 0);
	assert(write_count == 2);
	expect_write(0, base + 0x834U, 0x02000200U);
	expect_write(1, base + 0x830U, 0x00400040U);

	puts("rk3588 peripheral clock test: pass");
	return 0;
}
