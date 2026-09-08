/* SPDX-License-Identifier: MIT */
#include "rk3588_peripheral_service.h"

#include <stddef.h>
#include <string.h>

#define SERVICE_TIMEOUT_US 100000U
#define SERVICE_UNAVAILABLE (-19)
#define SERVICE_INVALID (-22)

static uint32_t load_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
	       ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void store_u32(uint8_t *p, uint32_t value)
{
	p[0] = value; p[1] = value >> 8U;
	p[2] = value >> 16U; p[3] = value >> 24U;
}

int lzamp_peripheral_service_execute(struct lzamp_peripheral_service *service,
				     const struct mailmsg_message *request,
				     uint8_t result[MAILMSG_PERIPH_RESULT_BYTES])
{
	uint32_t op, arg0, arg1, arg2, length, value = 0U, out_length = 0U;
	uint8_t bytes[MAILMSG_PERIPH_INLINE_BYTES] = { 0 };
	struct lzamp_rk3588_uart *uart;
	size_t received = 0U;
	int status = 0;

	if (!service || !request || !result ||
	    request->type != MAILMSG_MSG_PERIPHERAL_REQUEST ||
	    request->length != MAILMSG_PERIPH_REQUEST_BYTES)
		return -1;
	op = load_u32(request->payload + MAILMSG_PERIPH_OP_OFFSET);
	arg0 = load_u32(request->payload + MAILMSG_PERIPH_ARG0_OFFSET);
	arg1 = load_u32(request->payload + MAILMSG_PERIPH_ARG1_OFFSET);
	arg2 = load_u32(request->payload + MAILMSG_PERIPH_ARG2_OFFSET);
	length = load_u32(request->payload + MAILMSG_PERIPH_LENGTH_OFFSET);
	if (length > MAILMSG_PERIPH_INLINE_BYTES)
		status = SERVICE_INVALID;
	else switch (op) {
	case MAILMSG_PERIPH_GPIO_CONFIG:
		if (arg1 > 1U || arg2 > 1U)
			status = SERVICE_INVALID;
		else status = service->spi_profile ? SERVICE_UNAVAILABLE :
			arg1 ? lzamp_gpio3_configure_output(service->gpio, arg0, arg2) :
				lzamp_gpio3_configure_input(service->gpio, arg0);
		break;
	case MAILMSG_PERIPH_GPIO_WRITE:
		if (arg1 > 1U)
			status = SERVICE_INVALID;
		else status = service->spi_profile ? SERVICE_UNAVAILABLE :
			lzamp_gpio3_set(service->gpio, arg0, arg1);
		break;
	case MAILMSG_PERIPH_GPIO_READ: {
		int level = 0;
		status = service->spi_profile ? SERVICE_UNAVAILABLE :
			lzamp_gpio3_get(service->gpio, arg0, &level);
		value = (uint32_t)level;
		break;
	}
	case MAILMSG_PERIPH_UART_WRITE:
		uart = arg0 == 5U ? service->uart5 : arg0 == 7U ? service->uart7 : NULL;
		status = uart ? lzamp_rk3588_uart_write(uart,
			request->payload + MAILMSG_PERIPH_DATA_OFFSET, length,
			SERVICE_TIMEOUT_US) : SERVICE_INVALID;
		break;
	case MAILMSG_PERIPH_UART_READ:
		uart = arg0 == 5U ? service->uart5 : arg0 == 7U ? service->uart7 : NULL;
		if (!uart || !arg1 || arg1 > MAILMSG_PERIPH_INLINE_BYTES)
			status = SERVICE_INVALID;
		else {
			status = lzamp_rk3588_uart_read(uart, bytes, arg1, &received,
							 SERVICE_TIMEOUT_US);
			out_length = received;
		}
		break;
	case MAILMSG_PERIPH_I2C_WRITE:
		status = service->spi_profile ? SERVICE_UNAVAILABLE : arg0 > 0x7fU ?
			SERVICE_INVALID :
			lzamp_rk3588_i2c_write(service->i2c7, arg0,
				request->payload + MAILMSG_PERIPH_DATA_OFFSET,
				length, SERVICE_TIMEOUT_US);
		break;
	case MAILMSG_PERIPH_I2C_READ:
		if (service->spi_profile || arg0 > 0x7fU || !arg1 || arg1 > sizeof(bytes))
			status = service->spi_profile ? SERVICE_UNAVAILABLE : SERVICE_INVALID;
		else {
			status = lzamp_rk3588_i2c_read(service->i2c7, arg0, bytes,
						       arg1, SERVICE_TIMEOUT_US);
			out_length = status ? 0U : arg1;
		}
		break;
	case MAILMSG_PERIPH_I2C_WRITE_READ:
		if (service->spi_profile || arg0 > 0x7fU || !length || length > 3U ||
		    !arg1 || arg1 > sizeof(bytes))
			status = service->spi_profile ? SERVICE_UNAVAILABLE : SERVICE_INVALID;
		else {
			status = lzamp_rk3588_i2c_write_read(service->i2c7, arg0,
				request->payload + MAILMSG_PERIPH_DATA_OFFSET, length,
				bytes, arg1, SERVICE_TIMEOUT_US);
			out_length = status ? 0U : arg1;
		}
		break;
	case MAILMSG_PERIPH_SPI_TRANSFER:
		if (!service->spi_profile || !length || arg1 > 3U)
			status = service->spi_profile ? SERVICE_INVALID : SERVICE_UNAVAILABLE;
		else {
			status = lzamp_rk3588_spi_set_mode(service->spi0, arg1);
			if (!status)
				status = lzamp_rk3588_spi_transfer(service->spi0, arg0,
				request->payload + MAILMSG_PERIPH_DATA_OFFSET,
				bytes, length, SERVICE_TIMEOUT_US);
			out_length = status ? 0U : length;
		}
		break;
	case MAILMSG_PERIPH_PWM_SET:
		status = arg2 > 1U ? SERVICE_INVALID :
			lzamp_rk3588_pwm_set(service->pwm7, arg0, arg1, arg2);
		break;
	case MAILMSG_PERIPH_PWM_STOP:
		status = lzamp_rk3588_pwm_stop(service->pwm7);
		break;
	case MAILMSG_PERIPH_ADC_READ:
		status = lzamp_rk3588_reset_pulse(service->reset,
						 LZAMP_RESET_SARADC);
		if (!status)
			status = lzamp_rk3588_saradc_read(service->saradc, arg0,
							 &value, SERVICE_TIMEOUT_US);
		break;
	default:
		status = SERVICE_INVALID;
		break;
	}

	memset(result, 0, MAILMSG_PERIPH_RESULT_BYTES);
	store_u32(result + MAILMSG_PERIPH_RESULT_SEQUENCE_OFFSET, request->sequence);
	store_u32(result + MAILMSG_PERIPH_RESULT_STATUS_OFFSET, (uint32_t)status);
	store_u32(result + MAILMSG_PERIPH_RESULT_OP_OFFSET, op);
	store_u32(result + MAILMSG_PERIPH_RESULT_VALUE_OFFSET, value);
	store_u32(result + MAILMSG_PERIPH_RESULT_LENGTH_OFFSET, out_length);
	memcpy(result + MAILMSG_PERIPH_RESULT_DATA_OFFSET, bytes, out_length);
	return 0;
}
