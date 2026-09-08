/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "gpio3_polling.h"

struct write_record {
	uint32_t value;
	uintptr_t address;
};

static struct write_record writes[16];
static unsigned int write_count;
static uint32_t ext_port;

static uint32_t fake_read32(uintptr_t address)
{
	assert(address == 0xfec40070U);
	return ext_port;
}

static void fake_write32(uint32_t value, uintptr_t address)
{
	assert(write_count < 16U);
	writes[write_count].value = value;
	writes[write_count].address = address;
	write_count++;
}

static const struct lzamp_gpio3_mmio_ops fake_ops = {
	.read32 = fake_read32,
	.write32 = fake_write32,
};

int main(void)
{
	struct lzamp_gpio3 gpio;
	int value;

	assert(lzamp_gpio3_init(&gpio, 0xfec40000U, 0xfd5f0000U,
				&fake_ops) == LZAMP_GPIO3_OK);
	assert(lzamp_gpio3_configure_input(&gpio, LZAMP_GPIO3_PD1) == 0);
	assert(writes[0].address == 0xfd5f8078U);
	assert(writes[0].value == 0x00f00000U);
	assert(writes[1].address == 0xfec4000cU);
	assert(writes[1].value == 0x02000000U);

	assert(lzamp_gpio3_configure_output(&gpio, LZAMP_GPIO3_PD4, 0) == 0);
	assert(writes[2].address == 0xfd5f807cU);
	assert(writes[2].value == 0x000f0000U);
	assert(writes[3].address == 0xfec40004U);
	assert(writes[3].value == 0x10000000U);
	assert(writes[4].address == 0xfec4000cU);
	assert(writes[4].value == 0x10001000U);

	assert(lzamp_gpio3_set(&gpio, LZAMP_GPIO3_PD4, 1) == 0);
	assert(writes[5].value == 0x10001000U);
	assert(lzamp_gpio3_set(&gpio, LZAMP_GPIO3_PD5, 1) ==
	       LZAMP_GPIO3_NOT_OUTPUT);

	ext_port = 1U << LZAMP_GPIO3_PD5;
	assert(lzamp_gpio3_get(&gpio, LZAMP_GPIO3_PD5, &value) == 0);
	assert(value == 1);
	assert(lzamp_gpio3_configure_input(&gpio, 27U) ==
	       LZAMP_GPIO3_INVALID);

	puts("gpio3 polling test: pass");
	return 0;
}
