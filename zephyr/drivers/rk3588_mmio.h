/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_MMIO_H
#define LZAMP_RK3588_MMIO_H

#include <stdint.h>

struct lzamp_mmio_ops {
	uint32_t (*read32)(uintptr_t address);
	void (*write32)(uint32_t value, uintptr_t address);
	void (*delay_us)(uint32_t usec);
};

#endif
