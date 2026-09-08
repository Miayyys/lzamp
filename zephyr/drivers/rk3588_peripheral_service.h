/* SPDX-License-Identifier: MIT */
#ifndef LZAMP_RK3588_PERIPHERAL_SERVICE_H
#define LZAMP_RK3588_PERIPHERAL_SERVICE_H

#include "gpio3_polling.h"
#include "mailmsg.h"
#include "rk3588_i2c_polling.h"
#include "rk3588_pwm.h"
#include "rk3588_reset.h"
#include "rk3588_saradc.h"
#include "rk3588_spi_polling.h"
#include "rk3588_uart_polling.h"

struct lzamp_peripheral_service {
	struct lzamp_gpio3 *gpio;
	struct lzamp_rk3588_uart *uart5;
	struct lzamp_rk3588_uart *uart7;
	struct lzamp_rk3588_i2c *i2c7;
	struct lzamp_rk3588_spi *spi0;
	struct lzamp_rk3588_pwm *pwm7;
	struct lzamp_rk3588_saradc *saradc;
	struct lzamp_rk3588_reset_controller *reset;
	int spi_profile;
};

/* Returns a service status in result_status; malformed envelope returns -1. */
int lzamp_peripheral_service_execute(struct lzamp_peripheral_service *service,
				     const struct mailmsg_message *request,
				     uint8_t result[MAILMSG_PERIPH_RESULT_BYTES]);

#endif
