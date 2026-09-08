/* SPDX-License-Identifier: MIT */
/* Minimal Linux-to-Zephyr-to-Linux shared-memory PING responder. */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/cache.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#if defined(MAILMSG_TEST_SELF_OFF_VALUE) || defined(MAILMSG_ENABLE_STOP_CONTROL)
#include <zephyr/drivers/pm_cpu_ops.h>
#endif

#include "mailmsg_endpoint.h"
#include "mailmsg_memory_layout.h"
#include "gpio3_polling.h"
#include "rk3588_i2c_polling.h"
#include "rk3588_peripheral_clock.h"
#include "rk3588_peripheral_service.h"
#include "rk3588_pinctrl.h"
#include "rk3588_pwm.h"
#include "rk3588_reset.h"
#include "rk3588_saradc.h"
#include "rk3588_spi_polling.h"
#include "rk3588_uart_polling.h"

#define AMP_STATUS_ADDR       LZAMP_MAILMSG_CONTROL_ADDR
#define AMP_REQ_PAYLOAD_ADDR  (AMP_STATUS_ADDR + 0x40UL)
#define AMP_REQ_COMMIT_ADDR   (AMP_STATUS_ADDR + 0x80UL)
#define AMP_RSP_PAYLOAD_ADDR  (AMP_STATUS_ADDR + 0xc0UL)
#define AMP_RSP_COMMIT_ADDR   (AMP_STATUS_ADDR + 0x100UL)
#define AMP_MBOX_OBS_ADDR     (AMP_STATUS_ADDR + 0x140UL)
#define AMP_MAILMSG_TX_FULL_OBS_ADDR (AMP_STATUS_ADDR + 0x180UL)
#define AMP_MAILMSG_WORKER_OBS_ADDR (AMP_STATUS_ADDR + 0x1c0UL)
#define AMP_QUEUE_ADDR        (AMP_STATUS_ADDR + 0x200UL)
#define AMP_CACHE_LINE_SIZE   64U

#define RK3588_MAILBOX0_ADDR  0xfec60000UL
#define RK3588_MAILBOX_SIZE   0x200UL
#define RK3588_MAILBOX_CONTROLLERS 1U
#define RK3588_MAILBOX_CHANNELS 4U
#define MAILBOX_A2B_STATUS    0x04UL
#define MAILBOX_A2B_INTEN     0x00UL
#define MAILBOX_A2B_CMD(ch)   (0x08UL + (ch) * 8UL)
#define MAILBOX_A2B_DAT(ch)   (0x0cUL + (ch) * 8UL)
#define MAILBOX_TX_CHANNEL    3U
#define MAILBOX_B2A_STATUS    0x2cUL
#define MAILBOX_B2A_CMD(ch)   (0x30UL + (ch) * 8UL)
#define MAILBOX_B2A_DAT(ch)   (0x34UL + (ch) * 8UL)
#define MAILBOX_RX_CHANNEL    0U

#define RK3588_GPIO3_ADDR     0xfec40000UL
#define RK3588_GPIO3_SIZE     0x100UL
#define RK3588_IOC_ADDR       0xfd5f0000UL
#define RK3588_IOC_SIZE       0x10000UL
#define RK3588_CRU_ADDR       0xfd7c0000UL
#define RK3588_CRU_SIZE       0x1000UL
#define RK3588_UART5_ADDR     0xfeb80000UL
#define RK3588_UART7_ADDR     0xfeba0000UL
#define RK3588_UART_SIZE      0x100UL
#define RK3588_I2C7_ADDR      0xfec90000UL
#define RK3588_SPI0_ADDR      0xfeb00000UL
#define RK3588_BUS_SIZE       0x1000UL
#define RK3588_PWM7_ADDR      0xfebd0030UL
#define RK3588_PWM_SIZE       0x10UL
#define RK3588_SARADC_ADDR    0xfec10000UL
#define RK3588_SARADC_SIZE    0x1000UL

#define AMP_CMD_PING          1U
#define AMP_MBOX_RAW_TEST_CMD 0xa2b00000U
#define AMP_MBOX_PROTOCOL_CMD 0xa2b10000U
#define AMP_MBOX_PROTOCOL_ACK 0xb2a10000U
#define AMP_STATUS_OK         0
#define AMP_STATUS_BAD_CMD    (-1)
#define MARK_EL_HIGHEST       0x454c4849ULL /* "ELHI" */
#define MARK_EL2_INIT         0x454c3249ULL /* "EL2I" */
#define MARK_EL1_INIT         0x454c3149ULL /* "EL1I" */
#define MARK_MAIN             0x414d5031ULL /* "AMP1" */
#define MARK_HEARTBEAT        0x48420000ULL /* "HB" */
#define MARK_MBOX_OBS         0x4d424f58U   /* "MBOX" */
#define MARK_MBOX_ISR          0x4d495352U   /* "MISR" */
#define MARK_A2B_INTEN_ENABLE  0x41324245U   /* "A2BE" */
#define MARK_B2A_TX            0x42324154U   /* "B2AT" */
#define MARK_GIC_ROUTES         0x52544553U   /* "RTES" */
#define MARK_MATRIX_ISR         0x4d345249U   /* "M4RI" */
#define MARK_A2B_CLEAR_PROBE    0x41434b43U   /* "ACKC" */
#define MARK_MAILMSG_BAD_CRC    0x43524345U   /* "CRCE" */
#define MARK_MAILMSG_INVALID    0x494e564cU   /* "INVL" */
#define MARK_CPU_STOPPING       0x53544f50ULL /* "STOP" */
#define MARK_CPU_STOP_FAILED    0x53544641ULL /* "STFA" */
#define MARK_GPIO_MAP_BEGIN     0x47334d30ULL /* "G3M0" */
#define MARK_GPIO_MAP_GPIO      0x47334d31ULL /* "G3M1" */
#define MARK_GPIO_MAP_IOC       0x47334d32ULL /* "G3M2" */
#define MARK_GPIO_INPUT_D1      0x47334931ULL /* "G3I1" */
#define MARK_GPIO_INPUT_D4      0x47334934ULL /* "G3I4" */
#define MARK_GPIO_INPUT_D5      0x47334935ULL /* "G3I5" */
#define MAILMSG_WORKER_OBSERVATION_MAGIC 0x4d455654U /* "MEVT" */
#define MAILMSG_WORKER_STACK_SIZE 2048U
#define MAILMSG_WORKER_DRAIN_QUOTA (MAILMSG_RING_SLOTS - 1U)

/* Test-only build override.  Keeping an A2B bit pending verifies that a
 * second queued frame maps to COALESCED rather than a data-plane failure. */
#ifndef MAILMSG_TEST_HOLD_A2B_USEC
#define MAILMSG_TEST_HOLD_A2B_USEC 0U
#endif

/* Test-only override: leave one chosen priority queued so the Linux writer
 * can exercise its own -ENOSPC path.  The default skips no priority. */
#ifndef MAILMSG_TEST_SKIP_PRIORITY
#define MAILMSG_TEST_SKIP_PRIORITY MAILMSG_PRIORITY_COUNT
#endif

#if defined(MAILMSG_TEST_SELF_OFF_VALUE) || defined(MAILMSG_ENABLE_STOP_CONTROL)
static void mailmsg_cpu_off_after_ready(void);

__attribute__((noinline)) static int mailmsg_arch_cpu_off(void)
{
	uint64_t sctlr;
	int ret;

	ret = sys_cache_data_flush_and_invd_all();
	if (ret)
		return ret;

	/* The image is linked PA == VA, so the short PSCI path remains reachable
	 * after translation is disabled.  Preserve I-cache and every unrelated
	 * SCTLR bit; only data cache and address translation are quiesced. */
	__asm__ volatile(
		"dsb sy\n"
		"mrs %0, sctlr_el1\n"
		"bic %0, %0, %1\n"
		"msr sctlr_el1, %0\n"
		"isb\n"
		"dsb sy\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		: "=&r" (sctlr)
		: "r" ((uint64_t)(SCTLR_C_BIT | SCTLR_M_BIT))
		: "memory");

	return pm_cpu_off();
}
#endif
void r1_amp_checkpoint_c(uint64_t mark);

#define RK3588_GICD_BASE       0xfe600000UL
#define RK3588_GICD_SIZE       0x10000UL
#define GICD_IROUTER           0x6000UL
#define RK3588_MBOX_A2B_IRQ    100U
#define RK3588_MBOX_A2B_IRQ_CH0 97U
#define RK3588_MBOX_A2B_IRQ_CH1 98U
#define RK3588_MBOX_A2B_IRQ_CH2 99U
#define RK3588_MBOX_A2B_IRQ_CH3 100U
#define RK3588_MBOX_A2B_IRQ_FOR(index) \
	(97U + (((index) / RK3588_MAILBOX_CHANNELS) * 8U) + \
	 ((index) % RK3588_MAILBOX_CHANNELS))
#define B2A_ACK_COMMAND         0xb2a00001U

struct amp_payload_line {
	uint32_t command;
	uint32_t value;
	int32_t status;
	uint32_t result;
	uint8_t reserved[AMP_CACHE_LINE_SIZE - 16U];
} __attribute__((aligned(AMP_CACHE_LINE_SIZE)));

struct amp_commit_line {
	uint64_t sequence;
	uint64_t sequence_inv;
	uint8_t reserved[AMP_CACHE_LINE_SIZE - 16U];
} __attribute__((aligned(AMP_CACHE_LINE_SIZE)));

struct amp_mbox_observation_line {
	uint32_t magic;
	uint32_t a2b_status;
	uint32_t command;
	uint32_t data;
	uint8_t reserved[AMP_CACHE_LINE_SIZE - 16U];
} __attribute__((aligned(AMP_CACHE_LINE_SIZE)));

struct mailmsg_worker_observation_line {
	uint32_t magic;
	uint32_t commit;
	uint32_t commit_inv;
	uint32_t irq_count;
	uint32_t wake_count;
	uint32_t drain_count;
	uint32_t message_count;
	uint32_t empty_wake_count;
	uint32_t last_pending;
	uint32_t pending_now;
	uint32_t last_priority;
	uint8_t reserved[AMP_CACHE_LINE_SIZE - 44U];
} __attribute__((aligned(AMP_CACHE_LINE_SIZE)));

_Static_assert(sizeof(struct amp_payload_line) == AMP_CACHE_LINE_SIZE,
	       "payload must occupy one cache line");
_Static_assert(sizeof(struct amp_commit_line) == AMP_CACHE_LINE_SIZE,
	       "commit must occupy one cache line");
_Static_assert(sizeof(struct amp_mbox_observation_line) == AMP_CACHE_LINE_SIZE,
	       "mailbox observation must occupy one cache line");
_Static_assert(sizeof(struct mailmsg_worker_observation_line) ==
	       AMP_CACHE_LINE_SIZE,
	       "worker observation must occupy one cache line");
_Static_assert(sizeof(struct mailmsg_tx_full_observation) == AMP_CACHE_LINE_SIZE,
	       "MailMsg TX-full observation must occupy one cache line");

static inline void amp_dsb(void)
{
	__asm__ volatile ("dsb sy" : : : "memory");
}

static inline uint64_t current_el(void)
{
	uint64_t value;

	__asm__ volatile ("mrs %0, CurrentEL" : "=r" (value));
	return value;
}

static void mailmsg_queue_publish(const void *addr, mailmsg_u32 len)
{
	(void)sys_cache_data_flush_range((void *)addr, len);
	amp_dsb();
}

static void mailmsg_queue_acquire(const void *addr, mailmsg_u32 len)
{
	(void)sys_cache_data_invd_range((void *)addr, len);
	amp_dsb();
}

static const struct mailmsg_memory_ops mailmsg_queue_memory_ops = {
	.publish = mailmsg_queue_publish,
	.acquire = mailmsg_queue_acquire,
};

static uint32_t mailmsg_payload_u32(const uint8_t payload[4])
{
	return (uint32_t)payload[0] |
	       ((uint32_t)payload[1] << 8) |
	       ((uint32_t)payload[2] << 16) |
	       ((uint32_t)payload[3] << 24);
}

static void mailmsg_store_payload_u32(uint8_t payload[4], uint32_t value)
{
	payload[0] = value;
	payload[1] = value >> 8;
	payload[2] = value >> 16;
	payload[3] = value >> 24;
}

static mm_reg_t mailbox[RK3588_MAILBOX_CONTROLLERS];
static mm_reg_t gicd;
static mm_reg_t gpio3_mmio;
static mm_reg_t ioc_mmio;
static mm_reg_t cru_mmio;
static mm_reg_t uart5_mmio;
static mm_reg_t uart7_mmio;
static mm_reg_t i2c7_mmio;
static mm_reg_t spi0_mmio;
static mm_reg_t pwm7_mmio;
static mm_reg_t saradc_mmio;
static struct lzamp_gpio3 gpio3_polling;
static struct lzamp_rk3588_cru peripheral_cru;
static struct lzamp_rk3588_reset_controller peripheral_reset;
static struct lzamp_rk3588_pinctrl peripheral_pinctrl;
static struct lzamp_rk3588_uart peripheral_uart5;
static struct lzamp_rk3588_uart peripheral_uart7;
static struct lzamp_rk3588_i2c peripheral_i2c7;
static struct lzamp_rk3588_spi peripheral_spi0;
static struct lzamp_rk3588_pwm peripheral_pwm7;
static struct lzamp_rk3588_saradc peripheral_saradc;
static struct lzamp_peripheral_service peripheral_service;
static bool peripherals_ready;
static atomic_t mailmsg_pending_priorities;
static atomic_t mailmsg_irq_count;
K_SEM_DEFINE(mailmsg_work_sem, 0, 1);
K_THREAD_STACK_DEFINE(mailmsg_worker_stack, MAILMSG_WORKER_STACK_SIZE);
static struct k_thread mailmsg_worker_thread;
static uint32_t mailmsg_rx_error_count;
static bool mailmsg_endpoint_ready;
#ifndef MAILMSG_TEST_SKIP_SESSION_READY
static bool mailmsg_session_ready_committed;
static bool mailmsg_session_ready_published;
#endif
static volatile struct amp_mbox_observation_line *const mailbox_observation =
	(volatile struct amp_mbox_observation_line *)AMP_MBOX_OBS_ADDR;
static volatile struct mailmsg_tx_full_observation *const mailmsg_tx_full_observation =
	(volatile struct mailmsg_tx_full_observation *)AMP_MAILMSG_TX_FULL_OBS_ADDR;
static volatile struct mailmsg_worker_observation_line *const
	mailmsg_worker_observation =
	(volatile struct mailmsg_worker_observation_line *)
	AMP_MAILMSG_WORKER_OBS_ADDR;
static volatile struct mailmsg_shared *const amp_queue =
	(volatile struct mailmsg_shared *)AMP_QUEUE_ADDR;
static struct mailmsg_endpoint cpu3_endpoint;
static uint32_t mailmsg_tx_full_commit;
static uint32_t mailmsg_tx_full_count;
static uint32_t mailmsg_worker_commit;
static uint32_t mailmsg_worker_wake_count;
static uint32_t mailmsg_worker_drain_count;
static uint32_t mailmsg_worker_message_count;
static uint32_t mailmsg_worker_empty_wake_count;

static uint32_t peripheral_read32(uintptr_t address)
{
	return sys_read32((mem_addr_t)address);
}

static void peripheral_write32(uint32_t value, uintptr_t address)
{
	sys_write32(value, (mem_addr_t)address);
	amp_dsb();
}

#ifndef LZAMP_BUILD_PIN_PROFILE_SPI0
static const struct lzamp_gpio3_mmio_ops gpio3_mmio_ops = {
	.read32 = peripheral_read32,
	.write32 = peripheral_write32,
};
#endif

static void peripheral_delay_us(uint32_t usec)
{
	k_busy_wait(usec);
}

static const struct lzamp_mmio_ops peripheral_mmio_ops = {
	.read32 = peripheral_read32,
	.write32 = peripheral_write32,
	.delay_us = peripheral_delay_us,
};

static const struct lzamp_rk3588_cru_ops peripheral_cru_ops = {
	.write32 = peripheral_write32,
};

#ifndef LZAMP_BUILD_PIN_PROFILE_SPI0
static void gpio3_prepare_owned_lines_as_inputs(void)
{
	if (lzamp_gpio3_init(&gpio3_polling, (uintptr_t)gpio3_mmio,
			     (uintptr_t)ioc_mmio, &gpio3_mmio_ops) !=
	    LZAMP_GPIO3_OK)
		return;

	(void)lzamp_gpio3_configure_input(&gpio3_polling, LZAMP_GPIO3_PD1);
	r1_amp_checkpoint_c(MARK_GPIO_INPUT_D1);
	(void)lzamp_gpio3_configure_input(&gpio3_polling, LZAMP_GPIO3_PD4);
	r1_amp_checkpoint_c(MARK_GPIO_INPUT_D4);
	(void)lzamp_gpio3_configure_input(&gpio3_polling, LZAMP_GPIO3_PD5);
	r1_amp_checkpoint_c(MARK_GPIO_INPUT_D5);
}
#endif

static int peripherals_initialize(void)
{
	int ret = 0;

	ret |= lzamp_rk3588_cru_init(&peripheral_cru, (uintptr_t)cru_mmio,
				      &peripheral_cru_ops);
	ret |= lzamp_rk3588_reset_init(&peripheral_reset, (uintptr_t)cru_mmio,
					&peripheral_mmio_ops);
	ret |= lzamp_rk3588_pinctrl_init(&peripheral_pinctrl, (uintptr_t)ioc_mmio,
					  &peripheral_mmio_ops);
	if (ret)
		return ret;

	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_UART5);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_UART5);
	ret |= lzamp_rk3588_pinctrl_uart5(&peripheral_pinctrl);
	ret |= lzamp_rk3588_uart_init(&peripheral_uart5, (uintptr_t)uart5_mmio,
				       &peripheral_mmio_ops, 24000000U, 115200U);
	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_UART7);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_UART7);
	ret |= lzamp_rk3588_pinctrl_uart7(&peripheral_pinctrl);
	ret |= lzamp_rk3588_uart_init(&peripheral_uart7, (uintptr_t)uart7_mmio,
				       &peripheral_mmio_ops, 24000000U, 115200U);

#ifdef LZAMP_BUILD_PIN_PROFILE_SPI0
	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_SPI0);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_SPI0);
	ret |= lzamp_rk3588_pinctrl_profile(&peripheral_pinctrl,
					     LZAMP_PIN_PROFILE_SPI0);
	ret |= lzamp_rk3588_spi_init(&peripheral_spi0, (uintptr_t)spi0_mmio,
				      &peripheral_mmio_ops, 24000000U, 1000000U, 0U);
#else
	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_I2C7);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_I2C7);
	ret |= lzamp_rk3588_pinctrl_profile(&peripheral_pinctrl,
					     LZAMP_PIN_PROFILE_I2C7_GPIO);
	ret |= lzamp_rk3588_i2c_init(&peripheral_i2c7, (uintptr_t)i2c7_mmio,
				      &peripheral_mmio_ops, 100000000U, 100000U);
	gpio3_prepare_owned_lines_as_inputs();
#endif

	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_PWM1);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_PWM1);
	ret |= lzamp_rk3588_pwm_init(&peripheral_pwm7, (uintptr_t)pwm7_mmio,
				      &peripheral_mmio_ops, 24000000U);
	ret |= lzamp_rk3588_pinctrl_pwm7(&peripheral_pinctrl);
	ret |= lzamp_rk3588_peripheral_clock_enable(&peripheral_cru,
						     LZAMP_RK3588_CLOCK_SARADC);
	ret |= lzamp_rk3588_reset_pulse(&peripheral_reset, LZAMP_RESET_SARADC);
	ret |= lzamp_rk3588_saradc_init(&peripheral_saradc,
					 (uintptr_t)saradc_mmio,
					 &peripheral_mmio_ops);

	peripheral_service = (struct lzamp_peripheral_service) {
		.gpio = &gpio3_polling, .uart5 = &peripheral_uart5,
		.uart7 = &peripheral_uart7, .i2c7 = &peripheral_i2c7,
		.spi0 = &peripheral_spi0, .pwm7 = &peripheral_pwm7,
		.saradc = &peripheral_saradc, .reset = &peripheral_reset,
#ifdef LZAMP_BUILD_PIN_PROFILE_SPI0
		.spi_profile = 1,
#endif
	};
	return ret;
}

static void mailmsg_publish_worker_observation(uint32_t last_pending,
					       uint32_t last_priority)
{
	uint32_t commit = ++mailmsg_worker_commit;

	if (!commit)
		commit = ++mailmsg_worker_commit;
	mailmsg_worker_observation->commit = 0U;
	mailmsg_worker_observation->magic = MAILMSG_WORKER_OBSERVATION_MAGIC;
	mailmsg_worker_observation->irq_count = atomic_get(&mailmsg_irq_count);
	mailmsg_worker_observation->wake_count = mailmsg_worker_wake_count;
	mailmsg_worker_observation->drain_count = mailmsg_worker_drain_count;
	mailmsg_worker_observation->message_count = mailmsg_worker_message_count;
	mailmsg_worker_observation->empty_wake_count =
		mailmsg_worker_empty_wake_count;
	mailmsg_worker_observation->last_pending = last_pending;
	mailmsg_worker_observation->pending_now =
		atomic_get(&mailmsg_pending_priorities);
	mailmsg_worker_observation->last_priority = last_priority;
	mailmsg_worker_observation->commit_inv = ~commit;
	amp_dsb();
	mailmsg_worker_observation->commit = commit;
	(void)sys_cache_data_flush_range((void *)mailmsg_worker_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
}

/*
 * This is test-service diagnostics, not a MailMsg transport policy.  CPU3
 * is its only writer; Linux reads a stable snapshot through commit/inverse.
 */
static void mailmsg_publish_tx_full_observation(uint32_t priority,
					uint32_t type, int32_t result)
{
	uint32_t commit = ++mailmsg_tx_full_commit;

	if (!commit)
		commit = ++mailmsg_tx_full_commit;
	mailmsg_tx_full_observation->commit = 0U;
	mailmsg_tx_full_observation->magic = MAILMSG_TX_FULL_OBSERVATION_MAGIC;
	mailmsg_tx_full_observation->full_count = mailmsg_tx_full_count;
	mailmsg_tx_full_observation->last_priority = priority;
	mailmsg_tx_full_observation->last_type = type;
	mailmsg_tx_full_observation->last_result = result;
	mailmsg_tx_full_observation->commit_inv = ~commit;
	amp_dsb();
	mailmsg_tx_full_observation->commit = commit;
	(void)sys_cache_data_flush_range((void *)mailmsg_tx_full_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
}

static void mailmsg_reset_tx_full_observation(void)
{
	mailmsg_tx_full_count = 0U;
	mailmsg_tx_full_commit = 0U;
	mailmsg_publish_tx_full_observation(MAILMSG_PRIORITY_COUNT,
					   MAILMSG_MSG_NOP, MAILMSG_RING_OK);
}

static void mailmsg_record_tx_full(uint32_t priority, uint32_t type,
				   const struct mailmsg_send_result *result)
{
	if (!result || result->queue_result != MAILMSG_RING_FULL)
		return;

	mailmsg_tx_full_count++;
	mailmsg_publish_tx_full_observation(priority, type,
					   result->queue_result);
}

static int mailmsg_cpu3_notify(void *context,
			       enum mailmsg_priority priority)
{
	uint32_t pending;

	(void)context;
	pending = sys_read32(mailbox[0] + MAILBOX_B2A_STATUS);
	if (pending & BIT(priority))
		return MAILMSG_NOTIFY_COALESCED;

	sys_write32(AMP_MBOX_PROTOCOL_ACK | priority,
		    mailbox[0] + MAILBOX_B2A_CMD(priority));
	sys_write32(priority, mailbox[0] + MAILBOX_B2A_DAT(priority));
	amp_dsb();
	return MAILMSG_NOTIFY_SENT;
}

static const struct mailmsg_notify_ops mailmsg_cpu3_notify_ops = {
	.notify = mailmsg_cpu3_notify,
};

static const struct mailmsg_notify_endpoint mailmsg_cpu3_notify_endpoint = {
	.ops = &mailmsg_cpu3_notify_ops,
};

static bool mailmsg_send_feedback(uint32_t priority, uint32_t type,
				  uint32_t peer_sequence, uint32_t status)
{
	struct mailmsg_send_result result;
	uint8_t payload[MAILMSG_FEEDBACK_BYTES] = { 0 };
	int ret;

	mailmsg_store_payload_u32(&payload[MAILMSG_FEEDBACK_SEQUENCE_OFFSET],
				  peer_sequence);
	mailmsg_store_payload_u32(&payload[MAILMSG_FEEDBACK_STATUS_OFFSET],
				  status);
	ret = mailmsg_endpoint_send(&cpu3_endpoint,
				    (enum mailmsg_priority)priority, type,
				    payload, sizeof(payload), &result);
	if (ret)
		mailmsg_record_tx_full(priority, type, &result);
	return ret == MAILMSG_ENDPOINT_OK;
}

/* STOP_READY must have both a committed frame and an accepted doorbell before
 * CPU3 goes offline.  A regular endpoint send reports queue admission even
 * if its notification backend failed, which is insufficient for this edge. */
static bool mailmsg_send_stop_feedback(uint32_t type, uint32_t peer_sequence,
				       uint32_t status)
{
	struct mailmsg_send_result result;
	uint8_t payload[MAILMSG_FEEDBACK_BYTES] = { 0 };
	int ret;

	mailmsg_store_payload_u32(&payload[MAILMSG_FEEDBACK_SEQUENCE_OFFSET],
				  peer_sequence);
	mailmsg_store_payload_u32(&payload[MAILMSG_FEEDBACK_STATUS_OFFSET], status);
	ret = mailmsg_endpoint_send(&cpu3_endpoint, MAILMSG_PRIO_CRITICAL, type,
				    payload, sizeof(payload), &result);
	if (ret) {
		mailmsg_record_tx_full(MAILMSG_PRIO_CRITICAL, type, &result);
		return false;
	}

	return mailmsg_notify_accepted(result.notify_result);
}

/* Publish the session identity only after CPU3 has bound the shared ABI and
 * enabled every A2B interrupt.  Linux keeps user traffic closed until this
 * exact generation/version pair arrives on p0. */
#ifndef MAILMSG_TEST_SKIP_SESSION_READY
static void mailmsg_service_session_ready(void)
{
	struct mailmsg_send_result result;
	uint8_t payload[MAILMSG_SESSION_BYTES] = { 0 };
	int ret;

	if (!mailmsg_endpoint_ready || mailmsg_session_ready_published)
		return;
	/* If the frame was committed but its doorbell failed, retry only the
	 * notification.  Enqueuing another READY would leave a duplicate p0
	 * control frame after Linux has already opened the data plane. */
	if (mailmsg_session_ready_committed) {
		ret = mailmsg_cpu3_notify(NULL, MAILMSG_PRIO_CRITICAL);
		if (mailmsg_notify_accepted(ret))
			mailmsg_session_ready_published = true;
		return;
	}

	mailmsg_store_payload_u32(&payload[MAILMSG_SESSION_GENERATION_OFFSET],
				  cpu3_endpoint.generation);
	mailmsg_store_payload_u32(&payload[MAILMSG_SESSION_VERSION_OFFSET],
				  MAILMSG_PROTOCOL_VERSION);
	ret = mailmsg_endpoint_send(&cpu3_endpoint, MAILMSG_PRIO_CRITICAL,
				    MAILMSG_MSG_SESSION_READY, payload,
				    sizeof(payload), &result);
	if (ret) {
		mailmsg_record_tx_full(MAILMSG_PRIO_CRITICAL,
				       MAILMSG_MSG_SESSION_READY, &result);
		return;
	}
	mailmsg_session_ready_committed = true;
	mailmsg_session_ready_published =
		mailmsg_notify_accepted(result.notify_result);
}
#endif

static bool mailmsg_send_pong(uint32_t priority,
			      const struct mailmsg_message *request)
{
	struct mailmsg_send_result result;
	uint8_t payload[4];
	uint32_t type;
	int ret;

	type = request->type == MAILMSG_MSG_PING ?
		MAILMSG_MSG_PONG : MAILMSG_MSG_NOP;
	mailmsg_store_payload_u32(payload,
		mailmsg_payload_u32(request->payload) + 1U);
	ret = mailmsg_endpoint_send(&cpu3_endpoint,
				    (enum mailmsg_priority)priority, type,
				    payload, sizeof(payload), &result);
	if (ret)
		mailmsg_record_tx_full(priority, type, &result);
	return ret == MAILMSG_ENDPOINT_OK;
}

static bool mailmsg_send_gpio_read_result(uint32_t priority,
					  const struct mailmsg_message *request,
					  uint32_t line, uint32_t value)
{
	struct mailmsg_send_result result;
	uint8_t payload[MAILMSG_GPIO_READ_RESULT_BYTES] = { 0 };
	int ret;

	mailmsg_store_payload_u32(&payload[MAILMSG_GPIO_RESULT_SEQUENCE_OFFSET],
				 request->sequence);
	mailmsg_store_payload_u32(&payload[MAILMSG_GPIO_RESULT_LINE_OFFSET], line);
	mailmsg_store_payload_u32(&payload[MAILMSG_GPIO_RESULT_VALUE_OFFSET], value);
	ret = mailmsg_endpoint_send(&cpu3_endpoint,
				    (enum mailmsg_priority)priority,
				    MAILMSG_MSG_GPIO_READ_RESULT,
				    payload, sizeof(payload), &result);
	if (ret)
		mailmsg_record_tx_full(priority, MAILMSG_MSG_GPIO_READ_RESULT,
				       &result);
	return ret == MAILMSG_ENDPOINT_OK;
}

static bool mailmsg_send_peripheral_result(uint32_t priority,
				   const uint8_t payload[MAILMSG_PERIPH_RESULT_BYTES])
{
	struct mailmsg_send_result result;
	int ret;

	ret = mailmsg_endpoint_send(&cpu3_endpoint,
				    (enum mailmsg_priority)priority,
				    MAILMSG_MSG_PERIPHERAL_RESULT, payload,
				    MAILMSG_PERIPH_RESULT_BYTES, &result);
	if (ret)
		mailmsg_record_tx_full(priority, MAILMSG_MSG_PERIPHERAL_RESULT,
				       &result);
	return ret == MAILMSG_ENDPOINT_OK;
}

static void mailmsg_enable_a2b_channels(void)
{
	uint32_t controller, before = 0, after = 0;

	for (controller = 0; controller < RK3588_MAILBOX_CONTROLLERS; controller++) {
		before = sys_read32(mailbox[controller] + MAILBOX_A2B_INTEN);
		sys_write32(before | GENMASK(3, 0),
			    mailbox[controller] + MAILBOX_A2B_INTEN);
		after = sys_read32(mailbox[controller] + MAILBOX_A2B_INTEN);
	}
	amp_dsb();
	mailbox_observation->a2b_status = before;
	mailbox_observation->command = after;
	mailbox_observation->data = RK3588_MAILBOX_CONTROLLERS;
	mailbox_observation->magic = MARK_A2B_INTEN_ENABLE;
	(void)sys_cache_data_flush_range((void *)mailbox_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
}

/* Read-only preflight: raw 100 is the proven mailbox0 ch3 A2B line. */
static void mailmsg_snapshot_gic_routes(void)
{
	mailbox_observation->a2b_status =
		sys_read32(gicd + GICD_IROUTER + RK3588_MBOX_A2B_IRQ * 8U);
	mailbox_observation->command = RK3588_MBOX_A2B_IRQ;
	mailbox_observation->data = 0;
	mailbox_observation->magic = MARK_GIC_ROUTES;
	(void)sys_cache_data_flush_range((void *)mailbox_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
}

/*
 * Test application only: turn each received PING into a PONG.  MailMsg does
 * not define this scan order, a quota, or a fairness policy; a real CPU3
 * application owns those scheduling decisions.  The ISR remains only a
 * doorbell/ack path, and queue consumption stays outside IRQ context.
 */
static uint32_t mailmsg_pingpong_test_service(uint32_t pending_priorities,
					      uint32_t *last_priority,
					      uint32_t *reschedule)
{
	uint32_t priority;
	uint32_t messages = 0U;

	if (!mailmsg_endpoint_ready)
		return 0U;
#ifndef MAILMSG_TEST_SKIP_SESSION_READY
	if (!mailmsg_session_ready_published)
		return 0U;
#endif

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		struct mailmsg_message request;
		uint32_t handled = 0U;
		int pop_ret;

		if (!(pending_priorities & BIT(priority)) ||
		    priority == MAILMSG_TEST_SKIP_PRIORITY)
			continue;
		*last_priority = priority;

		while (handled < MAILMSG_WORKER_DRAIN_QUOTA) {
			pop_ret = mailmsg_endpoint_receive(
				&cpu3_endpoint, (enum mailmsg_priority)priority,
				&request);
			if (pop_ret) {
				if (pop_ret == MAILMSG_RING_BAD_CRC ||
				    pop_ret == MAILMSG_RING_INVALID) {
					uint32_t reason = pop_ret == MAILMSG_RING_BAD_CRC ?
						MAILMSG_NACK_BAD_CRC :
						MAILMSG_NACK_INVALID_FRAME;

					if (mailmsg_priority_is_reliable(priority))
						(void)mailmsg_send_feedback(
							priority, MAILMSG_MSG_NACK,
							request.sequence, reason);
					mailmsg_rx_error_count++;
					mailbox_observation->a2b_status = priority;
					mailbox_observation->command = reason;
					mailbox_observation->data = mailmsg_rx_error_count;
					mailbox_observation->magic =
						pop_ret == MAILMSG_RING_BAD_CRC ?
						MARK_MAILMSG_BAD_CRC : MARK_MAILMSG_INVALID;
					(void)sys_cache_data_flush_range(
						(void *)mailbox_observation,
						AMP_CACHE_LINE_SIZE);
					amp_dsb();
					messages++;
					handled++;
				}
				break;
			}
			messages++;
			handled++;

			if (priority == MAILMSG_PRIO_CRITICAL &&
			    request.type == MAILMSG_MSG_STOP_REQUEST) {
				if (request.length != 0U) {
					(void)mailmsg_send_stop_feedback(
						MAILMSG_MSG_STOP_REFUSED, request.sequence,
						MAILMSG_STOP_REFUSED_INVALID_REQUEST);
					continue;
				}

			#ifdef MAILMSG_ENABLE_STOP_CONTROL
				/* STOP_READY is the one control confirmation for this request;
				 * do not add the ordinary p0 ACK/NOP pair. */
				if (mailmsg_send_stop_feedback(MAILMSG_MSG_STOP_READY,
							       request.sequence, 0U))
					mailmsg_cpu_off_after_ready();
			#else
				(void)mailmsg_send_stop_feedback(MAILMSG_MSG_STOP_REFUSED,
							 request.sequence,
							 MAILMSG_STOP_REFUSED_BUSY);
			#endif
				continue;
			}

			#ifdef MAILMSG_TEST_SELF_OFF_VALUE
			if (priority == MAILMSG_PRIO_CRITICAL &&
			    request.type == MAILMSG_MSG_PING &&
			    request.length == sizeof(uint32_t) &&
			    mailmsg_payload_u32(request.payload) ==
				MAILMSG_TEST_SELF_OFF_VALUE)
				mailmsg_cpu_off_after_ready();
			#endif

			if (request.type == MAILMSG_MSG_PERIPHERAL_REQUEST) {
				uint8_t result[MAILMSG_PERIPH_RESULT_BYTES];

				if (priority != MAILMSG_PRIO_CONTROL || !peripherals_ready ||
				    lzamp_peripheral_service_execute(&peripheral_service,
							     &request, result)) {
					if (mailmsg_priority_is_reliable(priority))
						(void)mailmsg_send_feedback(
							priority, MAILMSG_MSG_NACK,
							request.sequence,
							MAILMSG_NACK_INVALID_FRAME);
					continue;
				}
				if (!mailmsg_send_feedback(priority, MAILMSG_MSG_ACK,
							   request.sequence, 0U))
					continue;
				(void)mailmsg_send_peripheral_result(priority, result);
				continue;
			}

			if (request.type == MAILMSG_MSG_GPIO_READ_REQUEST) {
				uint32_t line;
				int value;

				if (priority != MAILMSG_PRIO_CONTROL || !peripherals_ready ||
				    peripheral_service.spi_profile ||
				    request.length != MAILMSG_GPIO_READ_REQUEST_BYTES) {
					if (mailmsg_priority_is_reliable(priority))
						(void)mailmsg_send_feedback(
							priority, MAILMSG_MSG_NACK,
							request.sequence,
							MAILMSG_NACK_INVALID_FRAME);
					continue;
				}

				line = mailmsg_payload_u32(
					&request.payload[MAILMSG_GPIO_READ_LINE_OFFSET]);
				if (lzamp_gpio3_get(&gpio3_polling, line, &value) !=
				    LZAMP_GPIO3_OK) {
					(void)mailmsg_send_feedback(
						priority, MAILMSG_MSG_NACK,
						request.sequence,
						MAILMSG_NACK_INVALID_FRAME);
					continue;
				}

				if (!mailmsg_send_feedback(priority, MAILMSG_MSG_ACK,
						   request.sequence, 0U))
					continue;
				(void)mailmsg_send_gpio_read_result(priority, &request,
							    line, (uint32_t)value);
				continue;
			}

			if (mailmsg_priority_is_reliable(priority))
				(void)mailmsg_send_feedback(priority, MAILMSG_MSG_ACK,
						    request.sequence, 0U);
			(void)mailmsg_send_pong(priority, &request);
		}

		if (handled == MAILMSG_WORKER_DRAIN_QUOTA &&
		    mailmsg_endpoint_has_received(
			    &cpu3_endpoint,
			    (enum mailmsg_priority)priority) == 1)
			*reschedule |= BIT(priority);
	}

	return messages;
}

static void mailmsg_worker(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		uint32_t pending;
		uint32_t messages;
		uint32_t reschedule = 0U;
		uint32_t last_priority = MAILMSG_PRIORITY_COUNT;

		k_sem_take(&mailmsg_work_sem, K_FOREVER);
		mailmsg_worker_wake_count++;
		pending = atomic_set(&mailmsg_pending_priorities, 0);
		if (!pending) {
			mailmsg_worker_empty_wake_count++;
			mailmsg_publish_worker_observation(0U, last_priority);
			continue;
		}

		mailmsg_worker_drain_count++;
		messages = mailmsg_pingpong_test_service(pending,
							 &last_priority,
							 &reschedule);
		mailmsg_worker_message_count += messages;
		if (reschedule) {
			atomic_or(&mailmsg_pending_priorities, reschedule);
			k_sem_give(&mailmsg_work_sem);
		}
		mailmsg_publish_worker_observation(pending, last_priority);
	}
}

static void mailmsg_mailbox_a2b_matrix_isr(const void *arg)
{
	uint32_t index = (uintptr_t)arg;
	uint32_t controller = index / RK3588_MAILBOX_CHANNELS;
	uint32_t channel = index % RK3588_MAILBOX_CHANNELS;
	uint32_t status;
	uint32_t command;
	uint32_t data;
	uint32_t status_after;

	status = sys_read32(mailbox[controller] + MAILBOX_A2B_STATUS);
	command = sys_read32(mailbox[controller] + MAILBOX_A2B_CMD(channel));
	data = sys_read32(mailbox[controller] + MAILBOX_A2B_DAT(channel));

	/* Raw matrix probes get an immediate echo.  Protocol doorbells are
	 * answered by the test application's PING/PONG service after queue work. */
	if ((command & 0xfffffff0U) != AMP_MBOX_PROTOCOL_CMD) {
		sys_write32(0xb2a00000U | index,
			    mailbox[controller] + MAILBOX_B2A_CMD(channel));
		sys_write32(data, mailbox[controller] + MAILBOX_B2A_DAT(channel));
		amp_dsb();
	}

	/* Never enable this in a production image: it deliberately holds the
	 * corresponding level IRQ pending only long enough for a host test to
	 * submit a second frame on the same priority. */
	if (MAILMSG_TEST_HOLD_A2B_USEC &&
	    (command & 0xfffffff0U) == AMP_MBOX_PROTOCOL_CMD && !channel)
		k_busy_wait(MAILMSG_TEST_HOLD_A2B_USEC);

	/*
	 * The isolated probe established remote-side W1C on mailbox0 ch3.
	 * Acknowledge this channel before returning so a later doorbell can
	 * trigger the same IRQ again.  Keep recording before/after values so
	 * the repeated-use test has direct evidence for every transaction.
	 */
	sys_write32(BIT(channel), mailbox[controller] + MAILBOX_A2B_STATUS);
	amp_dsb();
	status_after = sys_read32(mailbox[controller] + MAILBOX_A2B_STATUS);

	mailbox_observation->a2b_status = status;
	mailbox_observation->command = status_after;
	mailbox_observation->data = (controller << 16) | BIT(channel);
	mailbox_observation->magic = MARK_A2B_CLEAR_PROBE;
	(void)sys_cache_data_flush_range((void *)mailbox_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
	if ((command & 0xfffffff0U) == AMP_MBOX_PROTOCOL_CMD ||
	    command == (AMP_MBOX_RAW_TEST_CMD | index)) {
		atomic_or(&mailmsg_pending_priorities, BIT(channel));
		atomic_inc(&mailmsg_irq_count);
		k_sem_give(&mailmsg_work_sem);
	}
}

static void mailmsg_send_b2a_ack(uint32_t sequence)
{
	/* Linux owns B2A_INTEN and B2A_STATUS acknowledgement for channel 0. */
	sys_write32(B2A_ACK_COMMAND,
		    mailbox[0] + MAILBOX_B2A_CMD(MAILBOX_RX_CHANNEL));
	sys_write32(sequence, mailbox[0] + MAILBOX_B2A_DAT(MAILBOX_RX_CHANNEL));
	amp_dsb();

	/* Best-effort write-side snapshot; Linux may acknowledge status immediately. */
	mailbox_observation->a2b_status =
		sys_read32(mailbox[0] + MAILBOX_B2A_STATUS);
	mailbox_observation->command = B2A_ACK_COMMAND;
	mailbox_observation->data = sequence;
	mailbox_observation->magic = MARK_B2A_TX;
	(void)sys_cache_data_flush_range((void *)mailbox_observation,
					 AMP_CACHE_LINE_SIZE);
	amp_dsb();
}

void r1_amp_checkpoint_c(uint64_t mark)
{
	volatile uint64_t *const status = (volatile uint64_t *)AMP_STATUS_ADDR;

	status[0] = mark;
	status[1] = current_el();
	__asm__ volatile ("dc cvac, %0" : : "r" (status) : "memory");
	amp_dsb();
}

void z_arm64_el_highest_plat_init(void)
{
	r1_amp_checkpoint_c(MARK_EL_HIGHEST);
}

void z_arm64_el2_plat_init(void)
{
	r1_amp_checkpoint_c(MARK_EL2_INIT);
}

void z_arm64_el1_plat_init(void)
{
	r1_amp_checkpoint_c(MARK_EL1_INIT);
}

#if defined(MAILMSG_TEST_SELF_OFF_VALUE) || defined(MAILMSG_ENABLE_STOP_CONTROL)
static void mailmsg_cpu_off_after_ready(void)
{
	unsigned int key;
	int ret;
	uint32_t controller;
	uint32_t index;
	uint32_t pending;

	/* Stop accepting doorbells before publishing the terminal state. */
	key = irq_lock();
	for (index = 0; index < RK3588_MAILBOX_CHANNELS; index++)
		irq_disable(RK3588_MBOX_A2B_IRQ_FOR(index));
	(void)key;

	/* The main loop may consume STOP_REQUEST before its level IRQ handler
	 * gets a chance to acknowledge A2B_STATUS.  Linux deliberately refuses
	 * rearm while any old-session doorbell is pending, so quiesce all four
	 * priority bits after IRQ disable and before PSCI CPU_OFF.  STOPPING has
	 * already closed the Linux data plane; no legitimate new A2B producer
	 * can race this final owner-side acknowledgement. */
	for (controller = 0; controller < RK3588_MAILBOX_CONTROLLERS;
	     controller++) {
		pending = sys_read32(mailbox[controller] + MAILBOX_A2B_STATUS) &
			  GENMASK(RK3588_MAILBOX_CHANNELS - 1U, 0U);
		if (pending)
			sys_write32(pending,
				    mailbox[controller] + MAILBOX_A2B_STATUS);
	}
	amp_dsb();
	for (controller = 0; controller < RK3588_MAILBOX_CONTROLLERS;
	     controller++)
		(void)sys_read32(mailbox[controller] + MAILBOX_A2B_STATUS);
	amp_dsb();

	r1_amp_checkpoint_c(MARK_CPU_STOPPING);

	/* PSCI CPU_OFF on this platform does not guarantee that CPU3's private
	 * cache/TLB state is discarded before a later CPU_ON.  Publish all dirty
	 * Zephyr state and invalidate local translations before entering firmware,
	 * so the next boot cannot reuse stale translation-table contents. */
	ret = mailmsg_arch_cpu_off();

	/* PSCI CPU_OFF must not return on success. */
	r1_amp_checkpoint_c(MARK_CPU_STOP_FAILED | ((uint32_t)(-ret) & 0xffffU));
	for (;;)
		__asm__ volatile ("wfe");
}
#endif

int main(void)
{
	volatile struct amp_payload_line *const request =
		(volatile struct amp_payload_line *)AMP_REQ_PAYLOAD_ADDR;
	volatile struct amp_commit_line *const request_commit =
		(volatile struct amp_commit_line *)AMP_REQ_COMMIT_ADDR;
	volatile struct amp_payload_line *const response =
		(volatile struct amp_payload_line *)AMP_RSP_PAYLOAD_ADDR;
	volatile struct amp_commit_line *const response_commit =
		(volatile struct amp_commit_line *)AMP_RSP_COMMIT_ADDR;
	uint64_t last_sequence = 0;
	uint64_t heartbeat = 0;
	uint32_t spin = 0;

	r1_amp_checkpoint_c(MARK_MAIN);
	/* Proven mailbox0 A2B group: one logical channel per priority. */
	IRQ_CONNECT(RK3588_MBOX_A2B_IRQ_FOR(0), 0, mailmsg_mailbox_a2b_matrix_isr, (void *)0, 0);
	IRQ_CONNECT(RK3588_MBOX_A2B_IRQ_FOR(1), 0, mailmsg_mailbox_a2b_matrix_isr, (void *)1, 0);
	IRQ_CONNECT(RK3588_MBOX_A2B_IRQ_FOR(2), 0, mailmsg_mailbox_a2b_matrix_isr, (void *)2, 0);
	IRQ_CONNECT(RK3588_MBOX_A2B_IRQ_FOR(3), 0, mailmsg_mailbox_a2b_matrix_isr, (void *)3, 0);
	device_map(&mailbox[0], RK3588_MAILBOX0_ADDR, RK3588_MAILBOX_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&gicd, RK3588_GICD_BASE, RK3588_GICD_SIZE,
		   K_MEM_CACHE_NONE);
	r1_amp_checkpoint_c(MARK_GPIO_MAP_BEGIN);
	device_map(&gpio3_mmio, RK3588_GPIO3_ADDR, RK3588_GPIO3_SIZE,
		   K_MEM_CACHE_NONE);
	r1_amp_checkpoint_c(MARK_GPIO_MAP_GPIO);
	device_map(&ioc_mmio, RK3588_IOC_ADDR, RK3588_IOC_SIZE,
		   K_MEM_CACHE_NONE);
	r1_amp_checkpoint_c(MARK_GPIO_MAP_IOC);
	device_map(&cru_mmio, RK3588_CRU_ADDR, RK3588_CRU_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&uart5_mmio, RK3588_UART5_ADDR, RK3588_UART_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&uart7_mmio, RK3588_UART7_ADDR, RK3588_UART_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&i2c7_mmio, RK3588_I2C7_ADDR, RK3588_BUS_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&spi0_mmio, RK3588_SPI0_ADDR, RK3588_BUS_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&pwm7_mmio, RK3588_PWM7_ADDR, RK3588_PWM_SIZE,
		   K_MEM_CACHE_NONE);
	device_map(&saradc_mmio, RK3588_SARADC_ADDR, RK3588_SARADC_SIZE,
		   K_MEM_CACHE_NONE);
	peripherals_ready = peripherals_initialize() == 0;
	mailmsg_endpoint_ready =
		mailmsg_endpoint_bind(&cpu3_endpoint,
				      (struct mailmsg_shared *)amp_queue,
				      &mailmsg_queue_memory_ops,
				      &mailmsg_cpu3_notify_endpoint,
			      MAILMSG_ENDPOINT_CPU3) ==
		MAILMSG_ENDPOINT_OK;
	mailmsg_reset_tx_full_observation();
	mailmsg_publish_worker_observation(0U, MAILMSG_PRIORITY_COUNT);
	k_thread_create(&mailmsg_worker_thread, mailmsg_worker_stack,
			K_THREAD_STACK_SIZEOF(mailmsg_worker_stack),
			mailmsg_worker, NULL, NULL, NULL,
			K_PRIO_COOP(0), 0, K_NO_WAIT);
	k_thread_name_set(&mailmsg_worker_thread, "mailmsg-worker");
	mailmsg_enable_a2b_channels();
	mailmsg_snapshot_gic_routes();
	irq_enable(RK3588_MBOX_A2B_IRQ_FOR(0)); irq_enable(RK3588_MBOX_A2B_IRQ_FOR(1));
	irq_enable(RK3588_MBOX_A2B_IRQ_FOR(2)); irq_enable(RK3588_MBOX_A2B_IRQ_FOR(3));
	if (mailmsg_endpoint_ready) {
#ifndef MAILMSG_TEST_SKIP_SESSION_READY
		mailmsg_service_session_ready();
#endif
	}
	for (;;) {
		uint64_t sequence;
		uint64_t sequence_inv;

		(void)sys_cache_data_invd_range((void *)request_commit,
						AMP_CACHE_LINE_SIZE);
		amp_dsb();
		sequence = request_commit->sequence;
		sequence_inv = request_commit->sequence_inv;

		if (sequence && sequence != last_sequence &&
		    sequence_inv == ~sequence) {
			uint32_t command;
			uint32_t value;

			(void)sys_cache_data_invd_range((void *)request,
							AMP_CACHE_LINE_SIZE);
			amp_dsb();
			command = request->command;
			value = request->value;

			response->command = command;
			response->value = value;
			response->status = command == AMP_CMD_PING ?
				AMP_STATUS_OK : AMP_STATUS_BAD_CMD;
			response->result = command == AMP_CMD_PING ? value + 1U : 0U;
			(void)sys_cache_data_flush_range((void *)response,
							 AMP_CACHE_LINE_SIZE);
			amp_dsb();

			response_commit->sequence = sequence;
			response_commit->sequence_inv = ~sequence;
			(void)sys_cache_data_flush_range((void *)response_commit,
							 AMP_CACHE_LINE_SIZE);
			amp_dsb();
			mailmsg_send_b2a_ack((uint32_t)sequence);
			last_sequence = sequence;
		}

#ifndef MAILMSG_TEST_SKIP_SESSION_READY
		mailmsg_service_session_ready();
#endif

		if (++spin == 1000000U) {
			r1_amp_checkpoint_c(MARK_HEARTBEAT |
					    (heartbeat++ & 0xffffU));
			spin = 0;
		}
	}
}
