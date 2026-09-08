/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rk3588_i2c_polling.h"
#include "rk3588_pinctrl.h"
#include "rk3588_pwm.h"
#include "rk3588_peripheral_service.h"
#include "rk3588_reset.h"
#include "rk3588_saradc.h"
#include "rk3588_spi_polling.h"
#include "rk3588_uart_polling.h"

#define BASE 0x10000000U

struct write_record { uintptr_t address; uint32_t value; };
static struct write_record writes[256];
static size_t write_count;
static uint32_t i2c_con;

static uint32_t mock_read(uintptr_t address)
{
	uint32_t offset = (uint32_t)(address - BASE);

	if (offset == 0x14U) return (1U << 0) | (1U << 5); /* UART LSR */
	if (offset == 0x24U) return 0U; /* SPI ready */
	if (offset == 0x800U) return 0x5aU; /* SPI RXDR */
	if (offset == 0x01cU)
		return (i2c_con & (1U << 4)) ? (1U << 5) :
		       ((i2c_con & (1U << 1)) ? (1U << 3) : (1U << 2));
	if (offset == 0x200U) return 0x44332211U;
	if (offset == 0x110U) return 1U;
	if (offset == 0x12cU) return 0x1abcU;
	return 0U;
}

static void mock_write(uint32_t value, uintptr_t address)
{
	assert(write_count < sizeof(writes) / sizeof(writes[0]));
	writes[write_count++] = (struct write_record){ address, value };
	if (address == BASE)
		i2c_con = value;
}

static void mock_delay(uint32_t usec) { (void)usec; }

static const struct lzamp_mmio_ops ops = {
	.read32 = mock_read, .write32 = mock_write, .delay_us = mock_delay,
};
static const struct lzamp_gpio3_mmio_ops gpio_ops = {
	.read32 = mock_read, .write32 = mock_write,
};

static void put_u32(uint8_t *p, uint32_t value)
{
	p[0] = value; p[1] = value >> 8U;
	p[2] = value >> 16U; p[3] = value >> 24U;
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
	       ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void reset_mock(void)
{
	memset(writes, 0, sizeof(writes));
	write_count = 0U;
	i2c_con = 0U;
}

static int saw_write(uintptr_t address, uint32_t value)
{
	size_t index;
	for (index = 0; index < write_count; index++)
		if (writes[index].address == address && writes[index].value == value)
			return 1;
	return 0;
}

static void test_pinctrl(void)
{
	struct lzamp_rk3588_pinctrl pinctrl;
	reset_mock();
	assert(lzamp_rk3588_pinctrl_init(&pinctrl, BASE, &ops) == 0);
	assert(lzamp_rk3588_pinctrl_uart5(&pinctrl) == 0);
	assert(saw_write(BASE + 0x8074U, 0x000f000aU));
	assert(saw_write(BASE + 0x8074U, 0x00f000a0U));
	assert(lzamp_rk3588_pinctrl_profile(&pinctrl, LZAMP_PIN_PROFILE_SPI0) == 0);
	assert(saw_write(BASE + 0x8078U, 0x00f00080U));
	assert(lzamp_rk3588_pinctrl_pwm7(&pinctrl) == 0);
	assert(saw_write(BASE + 0x400cU, 0x000f0008U));
	assert(saw_write(BASE + 0x8018U, 0x000f000bU));
}

static void test_reset(void)
{
	struct lzamp_rk3588_reset_controller reset;
	reset_mock();
	assert(lzamp_rk3588_reset_init(&reset, BASE, &ops) == 0);
	assert(lzamp_rk3588_reset_pulse(&reset, LZAMP_RESET_SPI0) == 0);
	assert(saw_write(BASE + 0xa38U, (1U << 22) | (1U << 6)));
	assert(saw_write(BASE + 0xa38U, (1U << 27) | (1U << 11)));
	assert(saw_write(BASE + 0xa38U, 1U << 22));
	assert(saw_write(BASE + 0xa38U, 1U << 27));
}

static void test_uart(void)
{
	struct lzamp_rk3588_uart uart;
	uint8_t tx[] = { 'O', 'K' }, rx[2];
	size_t received;
	reset_mock();
	assert(lzamp_rk3588_uart_init(&uart, BASE, &ops, 24000000U, 115200U) == 0);
	assert(saw_write(BASE, 13U));
	assert(lzamp_rk3588_uart_write(&uart, tx, sizeof(tx), 10U) == 0);
	assert(lzamp_rk3588_uart_read(&uart, rx, 1U, &received, 10U) == 0);
	assert(received == 1U);
}

static void test_spi(void)
{
	struct lzamp_rk3588_spi spi;
	uint8_t tx[2] = { 1U, 2U }, rx[2] = { 0 };
	reset_mock();
	assert(lzamp_rk3588_spi_init(&spi, BASE, &ops, 24000000U, 1000000U, 0U) == 0);
	assert(spi.baud_div == 24U);
	assert(lzamp_rk3588_spi_set_mode(&spi, 3U) == 0);
	assert((spi.ctrlr0 & ((1U << 6) | (1U << 7))) ==
	       ((1U << 6) | (1U << 7)));
	assert(lzamp_rk3588_spi_transfer(&spi, 0U, tx, rx, 2U, 10U) == 0);
	assert(rx[0] == 0x5aU && rx[1] == 0x5aU);
}

static void test_i2c(void)
{
	struct lzamp_rk3588_i2c i2c;
	uint8_t out[2] = { 0x10U, 0x20U }, prefix = 0x01U, in[4];
	reset_mock();
	assert(lzamp_rk3588_i2c_init(&i2c, BASE, &ops, 100000000U, 100000U) == 0);
	assert(saw_write(BASE + 0x04U, 0x003e003eU));
	assert(lzamp_rk3588_i2c_write(&i2c, 0x50U, out, 2U, 50U) == 0);
	assert(lzamp_rk3588_i2c_write_read(&i2c, 0x50U, &prefix, 1U,
				      in, sizeof(in), 50U) == 0);
	assert(in[0] == 0x11U && in[3] == 0x44U);
}

static void test_pwm_adc(void)
{
	struct lzamp_rk3588_pwm pwm;
	struct lzamp_rk3588_saradc adc;
	uint32_t sample;
	reset_mock();
	assert(lzamp_rk3588_pwm_init(&pwm, BASE, &ops, 24000000U) == 0);
	assert(lzamp_rk3588_pwm_set(&pwm, 1000000U, 250000U, 0) == 0);
	assert(saw_write(BASE + 0x04U, 24000U));
	assert(saw_write(BASE + 0x08U, 6000U));
	assert(lzamp_rk3588_pwm_stop(&pwm) == 0);
	reset_mock();
	assert(lzamp_rk3588_saradc_init(&adc, BASE, &ops) == 0);
	assert(lzamp_rk3588_saradc_read(&adc, 3U, &sample, 10U) == 0);
	assert(sample == 0xabcU);
}

static void test_peripheral_service(void)
{
	struct lzamp_gpio3 gpio;
	struct lzamp_rk3588_uart uart5 = { 0 }, uart7 = { 0 };
	struct lzamp_rk3588_i2c i2c = { 0 };
	struct lzamp_rk3588_spi spi = { 0 };
	struct lzamp_rk3588_pwm pwm;
	struct lzamp_rk3588_saradc adc;
	struct lzamp_rk3588_reset_controller reset;
	struct lzamp_peripheral_service service;
	struct mailmsg_message request = { .type = MAILMSG_MSG_PERIPHERAL_REQUEST,
		.sequence = 77U, .length = MAILMSG_PERIPH_REQUEST_BYTES };
	uint8_t result[MAILMSG_PERIPH_RESULT_BYTES];

	reset_mock();
	assert(lzamp_gpio3_init(&gpio, BASE, BASE, &gpio_ops) == 0);
	assert(lzamp_rk3588_uart_init(&uart5, BASE, &ops, 24000000U, 115200U) == 0);
	assert(lzamp_rk3588_uart_init(&uart7, BASE, &ops, 24000000U, 115200U) == 0);
	assert(lzamp_rk3588_i2c_init(&i2c, BASE, &ops, 100000000U, 100000U) == 0);
	assert(lzamp_rk3588_spi_init(&spi, BASE, &ops, 24000000U, 1000000U, 0U) == 0);
	assert(lzamp_rk3588_pwm_init(&pwm, BASE, &ops, 24000000U) == 0);
	assert(lzamp_rk3588_saradc_init(&adc, BASE, &ops) == 0);
	assert(lzamp_rk3588_reset_init(&reset, BASE, &ops) == 0);
	service = (struct lzamp_peripheral_service) {
		.gpio = &gpio, .uart5 = &uart5, .uart7 = &uart7, .i2c7 = &i2c,
		.spi0 = &spi, .pwm7 = &pwm, .saradc = &adc, .reset = &reset,
	};
	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET, MAILMSG_PERIPH_GPIO_CONFIG);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, LZAMP_GPIO3_PD1);
	put_u32(request.payload + MAILMSG_PERIPH_ARG1_OFFSET, 1U);
	put_u32(request.payload + MAILMSG_PERIPH_ARG2_OFFSET, 1U);
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert(get_u32(result + MAILMSG_PERIPH_RESULT_SEQUENCE_OFFSET) == 77U);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);

	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET, MAILMSG_PERIPH_ADC_READ);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, 3U);
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);
	assert(get_u32(result + MAILMSG_PERIPH_RESULT_VALUE_OFFSET) == 0xabcU);
	assert(saw_write(BASE + 0xa2cU, (1U << 30) | (1U << 14)));

	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET, MAILMSG_PERIPH_UART_WRITE);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, 5U);
	put_u32(request.payload + MAILMSG_PERIPH_LENGTH_OFFSET, 2U);
	request.payload[MAILMSG_PERIPH_DATA_OFFSET] = 'O';
	request.payload[MAILMSG_PERIPH_DATA_OFFSET + 1U] = 'K';
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);

	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET,
		MAILMSG_PERIPH_I2C_WRITE_READ);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, 0x50U);
	put_u32(request.payload + MAILMSG_PERIPH_ARG1_OFFSET, 4U);
	put_u32(request.payload + MAILMSG_PERIPH_LENGTH_OFFSET, 1U);
	request.payload[MAILMSG_PERIPH_DATA_OFFSET] = 1U;
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);
	assert(get_u32(result + MAILMSG_PERIPH_RESULT_LENGTH_OFFSET) == 4U);

	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET, MAILMSG_PERIPH_PWM_SET);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, 1000000U);
	put_u32(request.payload + MAILMSG_PERIPH_ARG1_OFFSET, 250000U);
	put_u32(request.payload + MAILMSG_PERIPH_ARG2_OFFSET, 0U);
	put_u32(request.payload + MAILMSG_PERIPH_LENGTH_OFFSET, 0U);
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);

	service.spi_profile = 1;
	put_u32(request.payload + MAILMSG_PERIPH_OP_OFFSET, MAILMSG_PERIPH_SPI_TRANSFER);
	put_u32(request.payload + MAILMSG_PERIPH_ARG0_OFFSET, 0U);
	put_u32(request.payload + MAILMSG_PERIPH_ARG1_OFFSET, 3U);
	put_u32(request.payload + MAILMSG_PERIPH_LENGTH_OFFSET, 2U);
	request.payload[MAILMSG_PERIPH_DATA_OFFSET] = 0xaaU;
	request.payload[MAILMSG_PERIPH_DATA_OFFSET + 1U] = 0x55U;
	assert(lzamp_peripheral_service_execute(&service, &request, result) == 0);
	assert((int32_t)get_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET) == 0);
	assert(get_u32(result + MAILMSG_PERIPH_RESULT_LENGTH_OFFSET) == 2U);
}

int main(void)
{
	test_pinctrl(); test_reset(); test_uart(); test_spi(); test_i2c();
	test_pwm_adc();
	test_peripheral_service();
	puts("rk3588 peripheral driver tests: PASS");
	return 0;
}
