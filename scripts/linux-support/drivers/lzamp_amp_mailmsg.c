// SPDX-License-Identifier: GPL-2.0-only
/*
 * LZAMP PSCI CPU lifecycle and MailMsg transport driver.
 *
 * This is deliberately a smoke-test mechanism, not a general AMP loader.
 * Users first write the exact Zephyr heartbeat image to the root-only binary
 * sysfs attribute.  Only a subsequent explicit "start" write may issue one
 * standard PSCI CPU_ON call for the Linux-excluded A55 core3.
 */

#include <asm/barrier.h>
#include <asm/cacheflush.h>

#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <soc/rockchip/rockchip-mailbox.h>

#include <uapi/linux/psci.h>

#include "../../../mailmsg/mailmsg_endpoint.h"
#include "../../../mailmsg/mailmsg_user.h"

#define R1_AMP_A55_CORE3_MPIDR	0x300UL
#define R1_AMP_MAGIC		0x414d5031ULL /* "AMP1" */
#define R1_AMP_STATUS_WORDS	2
#define R1_AMP_CACHE_LINE_SIZE	64
#define R1_AMP_REQ_PAYLOAD_OFF	0x40
#define R1_AMP_REQ_COMMIT_OFF	0x80
#define R1_AMP_RSP_PAYLOAD_OFF	0xc0
#define R1_AMP_RSP_COMMIT_OFF	0x100
#define R1_AMP_MBOX_OBS_OFF	0x140
#define R1_AMP_MAILMSG_TX_FULL_OBS_OFF	0x180
#define R1_AMP_MAILMSG_WORKER_OBS_OFF	0x1c0
#define R1_AMP_PROTOCOL_SIZE	0x200
#define R1_AMP_MAILMSG_OFF	0x200
#define R1_AMP_STATE_SIZE	(R1_AMP_MAILMSG_OFF + sizeof(struct mailmsg_shared))
#define R1_AMP_CMD_PING		1
#define R1_AMP_MAILMSG_DOORBELL	0xa2b10000U
#define R1_AMP_MBOX_A2B_STATUS	0x04
#define R1_AMP_MBOX_A2B_CMD(ch)	(0x08 + (ch) * 8)
#define R1_AMP_MBOX_A2B_DAT(ch)	(0x0c + (ch) * 8)
#define R1_AMP_MBOX_TX_CHANNEL	3
#define R1_AMP_MBOX_CONTROLLERS	1
#define R1_AMP_MBOX_CHANNELS_PER_CONTROLLER	4
#define R1_AMP_MBOX_CHANNELS	(R1_AMP_MBOX_CONTROLLERS * \
				 R1_AMP_MBOX_CHANNELS_PER_CONTROLLER)
#define R1_AMP_AFFINITY_MONITOR_MS	1000
#define R1_AMP_LIFECYCLE_TIMEOUT_MS	5000

struct r1_amp_cpu_on_data;

/* Local Linux-side lifecycle only; it is not part of the shared ABI. */
enum r1_amp_mailmsg_session_state {
	R1_AMP_MAILMSG_UNARMED,
	R1_AMP_MAILMSG_STARTING,
	R1_AMP_MAILMSG_START_TIMEOUT,
	R1_AMP_MAILMSG_ACTIVE,
	R1_AMP_MAILMSG_STOPPING,
	R1_AMP_MAILMSG_STOP_TIMEOUT,
	R1_AMP_MAILMSG_OFFLINE,
};

struct r1_amp_mailmsg_user_channel {
	struct miscdevice miscdev;
	struct r1_amp_cpu_on_data *parent;
	u32 priority;
	wait_queue_head_t rx_wait;
	atomic_t rx_generation;
	/* The shared reverse ring has exactly one Linux-side consumer. */
	atomic_t reader_open_count;
	bool registered;
};

struct r1_amp_mailmsg_user_file {
	struct r1_amp_mailmsg_user_channel *channel;
	bool reader_claimed;
};

/* One Linux mailbox client owns one complete, bidirectional V1 channel. */
struct r1_amp_mbox_channel {
	struct mbox_client client;
	struct mbox_chan *chan;
	struct rockchip_mbox_msg tx_msg;
	struct r1_amp_cpu_on_data *parent;
	u32 index;
	u32 controller;
	u32 controller_channel;
	atomic_t rx_count;
	u32 rx_last_cmd;
	u32 rx_last_data;
	int tx_ret;
	u32 tx_count;
	int notify_result;
	u32 notify_sent_count;
	u32 notify_coalesced_count;
	u32 notify_failed_count;
};

struct r1_amp_payload_line {
	u32 command;
	u32 value;
	s32 status;
	u32 result;
	u8 reserved[R1_AMP_CACHE_LINE_SIZE - 16];
};

struct r1_amp_commit_line {
	u64 sequence;
	u64 sequence_inv;
	u8 reserved[R1_AMP_CACHE_LINE_SIZE - 16];
};

/* Written only by Zephyr after it observes one Linux-to-Zephyr A2B event. */
struct r1_amp_mbox_observation_line {
	u32 magic;
	u32 a2b_status;
	u32 command;
	u32 data;
	u8 reserved[R1_AMP_CACHE_LINE_SIZE - 16];
};

#define R1_AMP_MAILMSG_WORKER_OBSERVATION_MAGIC	0x4d455654U /* "MEVT" */
struct r1_amp_mailmsg_worker_observation_line {
	u32 magic;
	u32 commit;
	u32 commit_inv;
	u32 irq_count;
	u32 wake_count;
	u32 drain_count;
	u32 message_count;
	u32 empty_wake_count;
	u32 last_pending;
	u32 pending_now;
	u32 last_priority;
	u8 reserved[R1_AMP_CACHE_LINE_SIZE - 44];
};

static_assert(sizeof(struct r1_amp_payload_line) == R1_AMP_CACHE_LINE_SIZE);
static_assert(sizeof(struct r1_amp_commit_line) == R1_AMP_CACHE_LINE_SIZE);
static_assert(sizeof(struct r1_amp_mbox_observation_line) ==
	      R1_AMP_CACHE_LINE_SIZE);
static_assert(sizeof(struct r1_amp_mailmsg_worker_observation_line) ==
	      R1_AMP_CACHE_LINE_SIZE);
static_assert(sizeof(struct mailmsg_tx_full_observation) ==
	      R1_AMP_CACHE_LINE_SIZE);

struct r1_amp_cpu_on_data {
	struct mutex lock;
	struct r1_amp_mbox_channel mbox[R1_AMP_MBOX_CHANNELS];
	void __iomem *mbox_regs[R1_AMP_MBOX_CONTROLLERS];
	u32 mbox_a2b_at_tx_status;
	u32 mbox_a2b_at_tx_cmd;
	u32 mbox_a2b_at_tx_data;
	void *carveout;
	phys_addr_t carveout_phys;
	resource_size_t carveout_size;
	u64 mpidr;
	u64 entry;
	u32 image_size;
	u32 status_offset;
	size_t image_written;
	int affinity_state;
	int cpu_on_ret;
	bool affinity_queried;
	bool affinity_seen_on;
	bool cpu_on_attempted;
	enum r1_amp_mailmsg_session_state mailmsg_state;
	struct delayed_work affinity_work;
	struct work_struct control_work;
	struct delayed_work lifecycle_timeout_work;
	u32 affinity_monitor_count;
	u32 affinity_monitor_error_count;
	u32 session_generation;
	u32 peer_generation;
	int session_result;
	u32 stop_request_sequence;
	u32 stop_reply_type;
	int stop_result;
	int stop_notify_result;
	unsigned long lifecycle_deadline;
	u32 notify_inject_priority;
	int notify_inject_errno;
	u32 notify_inject_remaining;
	bool lifecycle_notify_bypass;
	u64 next_sequence;
	int mailmsg_last_notify_result;
	struct mailmsg_endpoint mailmsg_endpoint;
	struct mailmsg_notify_endpoint mailmsg_notify_endpoint;
	struct r1_amp_mailmsg_user_channel mailmsg_user[MAILMSG_PRIORITY_COUNT];
	atomic_t mailmsg_user_open_count;
	bool mailmsg_user_removing;
	struct bin_attribute image_attr;
};

static void r1_amp_mailmsg_control_work(struct work_struct *work);
static void r1_amp_mailmsg_lifecycle_timeout_work(struct work_struct *work);
static int mailmsg_ring_result_to_errno(int result);
static u32 mailmsg_payload_u32(const mailmsg_u8 payload[4]);

static void r1_amp_mbox_rx_callback(struct mbox_client *client, void *message)
{
	struct r1_amp_mbox_channel *channel =
		container_of(client, struct r1_amp_mbox_channel, client);
	struct rockchip_mbox_msg *msg = message;

	/* Copy immediately: the mailbox controller owns the message storage. */
	if (msg) {
		WRITE_ONCE(channel->rx_last_cmd, msg->cmd);
		WRITE_ONCE(channel->rx_last_data, msg->data);
	}
	atomic_inc(&channel->rx_count);
	atomic_inc(&channel->parent->mailmsg_user[channel->index].rx_generation);
	wake_up_interruptible(&channel->parent->mailmsg_user[channel->index].rx_wait);
	if (READ_ONCE(channel->parent->mailmsg_state) ==
		R1_AMP_MAILMSG_STARTING ||
	    READ_ONCE(channel->parent->mailmsg_state) ==
		R1_AMP_MAILMSG_STOPPING)
		schedule_work(&channel->parent->control_work);
}

static const char *r1_amp_psci_state_name(int state)
{
	switch (state) {
	case PSCI_0_2_AFFINITY_LEVEL_ON:
		return "on";
	case PSCI_0_2_AFFINITY_LEVEL_OFF:
		return "off";
	case PSCI_0_2_AFFINITY_LEVEL_ON_PENDING:
		return "on-pending";
	default:
		return "error";
	}
}

static const char *r1_amp_mailmsg_state_name(
		enum r1_amp_mailmsg_session_state state)
{
	switch (state) {
	case R1_AMP_MAILMSG_UNARMED:
		return "unarmed";
	case R1_AMP_MAILMSG_STARTING:
		return "starting";
	case R1_AMP_MAILMSG_START_TIMEOUT:
		return "start-timeout";
	case R1_AMP_MAILMSG_ACTIVE:
		return "active";
	case R1_AMP_MAILMSG_STOPPING:
		return "stopping";
	case R1_AMP_MAILMSG_STOP_TIMEOUT:
		return "stop-timeout";
	case R1_AMP_MAILMSG_OFFLINE:
		return "offline";
	}

	return "unknown";
}

/* One gate for every data-plane entry point, including test-only sysfs
 * helpers.  Lifecycle control is the only owner while STARTING/STOPPING. */
static int r1_amp_mailmsg_data_gate_locked(struct r1_amp_cpu_on_data *data)
{
	switch (data->mailmsg_state) {
	case R1_AMP_MAILMSG_ACTIVE:
		return 0;
	case R1_AMP_MAILMSG_STARTING:
		return -EAGAIN;
	case R1_AMP_MAILMSG_START_TIMEOUT:
		return -ETIMEDOUT;
	case R1_AMP_MAILMSG_STOPPING:
		return -ESHUTDOWN;
	case R1_AMP_MAILMSG_STOP_TIMEOUT:
		return -ETIMEDOUT;
	case R1_AMP_MAILMSG_OFFLINE:
	case R1_AMP_MAILMSG_UNARMED:
		return -ENOLINK;
	}
	return -EIO;
}

/* Caller holds data->lock.  Wake every priority so blocking readers observe
 * the terminal endpoint state instead of sleeping for a mailbox IRQ that can
 * no longer arrive from the stopped CPU. */
static void r1_amp_mailmsg_mark_offline_locked(struct r1_amp_cpu_on_data *data)
{
	int priority;

	if (data->mailmsg_state != R1_AMP_MAILMSG_STARTING &&
	    data->mailmsg_state != R1_AMP_MAILMSG_START_TIMEOUT &&
	    data->mailmsg_state != R1_AMP_MAILMSG_ACTIVE &&
	    data->mailmsg_state != R1_AMP_MAILMSG_STOPPING &&
	    data->mailmsg_state != R1_AMP_MAILMSG_STOP_TIMEOUT)
		return;

	data->mailmsg_state = R1_AMP_MAILMSG_OFFLINE;
	cancel_delayed_work(&data->lifecycle_timeout_work);
	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		atomic_inc(&data->mailmsg_user[priority].rx_generation);
		wake_up_interruptible(&data->mailmsg_user[priority].rx_wait);
	}
}

/* Caller holds data->lock. */
static int r1_amp_refresh_affinity_locked(struct r1_amp_cpu_on_data *data)
{
	int state;

	state = psci_ops.affinity_info(data->mpidr, 0);
	data->affinity_state = state;
	data->affinity_queried = true;
	if (state == PSCI_0_2_AFFINITY_LEVEL_ON ||
	    state == PSCI_0_2_AFFINITY_LEVEL_ON_PENDING)
		data->affinity_seen_on = true;
	/* CPU_ON may return before firmware changes AFFINITY_INFO away from OFF.
	 * Require one observed ON/ON_PENDING state before treating a later OFF as
	 * a terminal peer shutdown. */
	if (!data->cpu_on_ret && data->affinity_seen_on &&
	    state == PSCI_0_2_AFFINITY_LEVEL_OFF)
		r1_amp_mailmsg_mark_offline_locked(data);

	return state;
}

static void r1_amp_affinity_work(struct work_struct *work)
{
	struct r1_amp_cpu_on_data *data =
		container_of(to_delayed_work(work), struct r1_amp_cpu_on_data,
			     affinity_work);
	int state;

	mutex_lock(&data->lock);
	if ((data->mailmsg_state != R1_AMP_MAILMSG_STARTING &&
	     data->mailmsg_state != R1_AMP_MAILMSG_START_TIMEOUT &&
	     data->mailmsg_state != R1_AMP_MAILMSG_ACTIVE &&
	     data->mailmsg_state != R1_AMP_MAILMSG_STOPPING &&
	     data->mailmsg_state != R1_AMP_MAILMSG_STOP_TIMEOUT) ||
	    data->mailmsg_user_removing)
		goto out;

	data->affinity_monitor_count++;
	state = r1_amp_refresh_affinity_locked(data);
	if (state < 0)
		data->affinity_monitor_error_count++;
	/* A coalesced lifecycle doorbell must not create a lost-wakeup window.
	 * While the lifecycle plane owns p0, periodically drain it as a fallback
	 * in addition to mailbox callbacks. */
	if (data->mailmsg_state == R1_AMP_MAILMSG_STARTING ||
	    data->mailmsg_state == R1_AMP_MAILMSG_STOPPING)
		schedule_work(&data->control_work);
	if ((data->mailmsg_state == R1_AMP_MAILMSG_STARTING ||
	     data->mailmsg_state == R1_AMP_MAILMSG_START_TIMEOUT ||
	     data->mailmsg_state == R1_AMP_MAILMSG_ACTIVE ||
	     data->mailmsg_state == R1_AMP_MAILMSG_STOPPING ||
	     data->mailmsg_state == R1_AMP_MAILMSG_STOP_TIMEOUT) &&
	    !data->mailmsg_user_removing)
		mod_delayed_work(system_wq, &data->affinity_work,
				 msecs_to_jiffies(R1_AMP_AFFINITY_MONITOR_MS));
out:
	mutex_unlock(&data->lock);
}

/* Caller holds data->lock.  A timeout is a terminal lifecycle result: late
 * control frames cannot silently reopen or complete the expired operation. */
static void r1_amp_mailmsg_set_timeout_locked(struct r1_amp_cpu_on_data *data)
{
	int priority;

	if (data->mailmsg_state == R1_AMP_MAILMSG_STARTING) {
		data->session_result = -ETIMEDOUT;
		data->mailmsg_state = R1_AMP_MAILMSG_START_TIMEOUT;
	} else if (data->mailmsg_state == R1_AMP_MAILMSG_STOPPING) {
		data->stop_result = -ETIMEDOUT;
		data->mailmsg_state = R1_AMP_MAILMSG_STOP_TIMEOUT;
	}
	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		atomic_inc(&data->mailmsg_user[priority].rx_generation);
		wake_up_interruptible(&data->mailmsg_user[priority].rx_wait);
	}
}

/* SESSION_READY and stop replies are consumed only by the kernel lifecycle
 * plane.  User traffic is closed in STARTING/STOPPING, so p0 has one reader. */
static void r1_amp_mailmsg_control_work(struct work_struct *work)
{
	struct r1_amp_cpu_on_data *data =
		container_of(work, struct r1_amp_cpu_on_data, control_work);
	struct mailmsg_message response;
	int ret;

	mutex_lock(&data->lock);
	if (data->mailmsg_state != R1_AMP_MAILMSG_STARTING &&
	    data->mailmsg_state != R1_AMP_MAILMSG_STOPPING)
		goto out;
	if (time_after_eq(jiffies, data->lifecycle_deadline)) {
		r1_amp_mailmsg_set_timeout_locked(data);
		cancel_delayed_work(&data->lifecycle_timeout_work);
		goto out;
	}

	for (;;) {
		ret = mailmsg_endpoint_receive(&data->mailmsg_endpoint,
					       MAILMSG_PRIO_CRITICAL, &response);
		if (ret == MAILMSG_RING_EMPTY || ret == MAILMSG_RING_INCOMPLETE)
			break;
		if (ret) {
			/* A stale-generation frame was consumed and released.  Keep
			 * draining for the current session's lifecycle response. */
			if (ret == MAILMSG_ENDPOINT_STALE_FRAME)
				continue;
			if (data->mailmsg_state == R1_AMP_MAILMSG_STARTING)
				data->session_result = mailmsg_ring_result_to_errno(ret);
			else
				data->stop_result = mailmsg_ring_result_to_errno(ret);
			/* Endpoint/session errors do not consume a ring entry.  Retrying
			 * under the same mutex would spin forever; leave the lifecycle
			 * timeout as the safe terminal observation. */
			if (ret <= MAILMSG_ENDPOINT_INVALID)
				break;
			continue;
		}
		if (time_after_eq(jiffies, data->lifecycle_deadline)) {
			r1_amp_mailmsg_set_timeout_locked(data);
			cancel_delayed_work(&data->lifecycle_timeout_work);
			break;
		}

		if (data->mailmsg_state == R1_AMP_MAILMSG_STARTING) {
			if (response.type != MAILMSG_MSG_SESSION_READY ||
			    response.length < MAILMSG_SESSION_BYTES ||
			    mailmsg_payload_u32(
				&response.payload[MAILMSG_SESSION_GENERATION_OFFSET]) !=
					data->session_generation ||
			    mailmsg_payload_u32(
				&response.payload[MAILMSG_SESSION_VERSION_OFFSET]) !=
					MAILMSG_PROTOCOL_VERSION)
				continue;

			data->peer_generation = data->session_generation;
			data->session_result = 0;
			/* A generation-matched frame is stronger evidence than a sampled
			 * PSCI state: CPU3 has executed the new image in this session. */
			data->affinity_seen_on = true;
			data->mailmsg_state = R1_AMP_MAILMSG_ACTIVE;
			cancel_delayed_work(&data->lifecycle_timeout_work);
			break;
		}

		if ((response.type != MAILMSG_MSG_STOP_READY &&
		     response.type != MAILMSG_MSG_STOP_REFUSED) ||
		    response.length < MAILMSG_FEEDBACK_BYTES ||
		    mailmsg_payload_u32(
				&response.payload[MAILMSG_FEEDBACK_SEQUENCE_OFFSET]) !=
				data->stop_request_sequence)
			continue;

		if (response.type == MAILMSG_MSG_STOP_READY) {
			data->stop_reply_type = response.type;
			data->stop_result = 0;
			/* STOP_READY ends the reply phase.  Give PSCI CPU_OFF its own
			 * bounded interval instead of reusing the original deadline. */
			data->lifecycle_deadline = jiffies +
				msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS);
			mod_delayed_work(system_wq, &data->lifecycle_timeout_work,
					 msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS));
			break;
		}

		/* A refusal leaves CPU3 running and reopens the normal data plane. */
		data->stop_reply_type = response.type;
		data->stop_result = -ECANCELED;
		data->mailmsg_state = R1_AMP_MAILMSG_ACTIVE;
		cancel_delayed_work(&data->lifecycle_timeout_work);
		break;
	}
out:
	mutex_unlock(&data->lock);
}

static void r1_amp_mailmsg_lifecycle_timeout_work(struct work_struct *work)
{
	struct r1_amp_cpu_on_data *data =
		container_of(to_delayed_work(work), struct r1_amp_cpu_on_data,
			     lifecycle_timeout_work);
	unsigned long remaining;

	mutex_lock(&data->lock);
	if (data->mailmsg_state != R1_AMP_MAILMSG_STARTING &&
	    data->mailmsg_state != R1_AMP_MAILMSG_STOPPING)
		goto out;
	if (time_before(jiffies, data->lifecycle_deadline)) {
		remaining = data->lifecycle_deadline - jiffies;
		mod_delayed_work(system_wq, &data->lifecycle_timeout_work,
				 remaining);
		goto out;
	}
	/* Avoid reporting a STOP timeout merely because the periodic affinity
	 * sampler has not yet observed a CPU that is already OFF. */
	if (data->mailmsg_state == R1_AMP_MAILMSG_STOPPING &&
	    data->stop_reply_type == MAILMSG_MSG_STOP_READY) {
		r1_amp_refresh_affinity_locked(data);
		if (data->mailmsg_state == R1_AMP_MAILMSG_OFFLINE)
			goto out;
	}

	r1_amp_mailmsg_set_timeout_locked(data);
out:
	mutex_unlock(&data->lock);
}

static void *r1_amp_status_ptr(struct r1_amp_cpu_on_data *data)
{
	return data->carveout + data->status_offset;
}

static void *r1_amp_protocol_ptr(struct r1_amp_cpu_on_data *data,
				unsigned int offset)
{
	return r1_amp_status_ptr(data) + offset;
}

static struct r1_amp_mbox_observation_line *
r1_amp_mbox_observation_ptr(struct r1_amp_cpu_on_data *data)
{
	return r1_amp_protocol_ptr(data, R1_AMP_MBOX_OBS_OFF);
}

static struct mailmsg_tx_full_observation *
r1_amp_mailmsg_tx_full_observation_ptr(struct r1_amp_cpu_on_data *data)
{
	return r1_amp_protocol_ptr(data, R1_AMP_MAILMSG_TX_FULL_OBS_OFF);
}

static struct r1_amp_mailmsg_worker_observation_line *
r1_amp_mailmsg_worker_observation_ptr(struct r1_amp_cpu_on_data *data)
{
	return r1_amp_protocol_ptr(data, R1_AMP_MAILMSG_WORKER_OBS_OFF);
}

static struct mailmsg_shared *mailmsg_linux_shared_ptr(struct r1_amp_cpu_on_data *data)
{
	return r1_amp_protocol_ptr(data, R1_AMP_MAILMSG_OFF);
}

static void lzamp_dcache_clean(const void *addr, size_t len)
{
	unsigned long start = (unsigned long)addr;

	dcache_clean_poc(start, start + len);
}

static void lzamp_dcache_invalidate(const void *addr, size_t len)
{
	unsigned long start = (unsigned long)addr;

	dcache_inval_poc(start, start + len);
}

static void mailmsg_linux_publish(const void *addr, mailmsg_u32 len)
{
	lzamp_dcache_clean(addr, len);
	dsb(sy);
}

static void mailmsg_linux_acquire(const void *addr, mailmsg_u32 len)
{
	lzamp_dcache_invalidate(addr, len);
	dsb(sy);
}

static const struct mailmsg_memory_ops mailmsg_linux_memory_ops = {
	.publish = mailmsg_linux_publish,
	.acquire = mailmsg_linux_acquire,
};

static u32 mailmsg_payload_u32(const mailmsg_u8 payload[4])
{
	return (u32)payload[0] | ((u32)payload[1] << 8) |
	       ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
}

static void mailmsg_store_payload_u32(mailmsg_u8 payload[4], u32 value)
{
	payload[0] = value;
	payload[1] = value >> 8;
	payload[2] = value >> 16;
	payload[3] = value >> 24;
}

/* Fault-injection helper only.  Normal traffic uses mailmsg_endpoint_send(). */
static int mailmsg_linux_ring_push(struct mailmsg_ring *ring,
				   struct mailmsg_message *message,
				   bool corrupt_payload)
{
	mailmsg_u32 producer = ring->producer;
	mailmsg_u32 next = (producer + 1U) % MAILMSG_RING_SLOTS;
	struct mailmsg_message *slot;

	if (!message->generation || !message->sequence ||
	    message->length > MAILMSG_PAYLOAD_BYTES)
		return -EINVAL;
	mailmsg_linux_acquire(&ring->consumer, sizeof(ring->consumer));
	if (next == ring->consumer)
		return -ENOSPC;

	slot = &ring->slot[producer];
	*slot = *message;
	slot->commit = 0;
	slot->crc32 = mailmsg_frame_crc32(slot);
	/*
	 * Test-only fault injection.  Corrupt the payload after calculating the
	 * CRC but before publishing either the frame or producer index.  Zephyr
	 * therefore cannot race ahead and consume an uncorrupted frame.
	 */
	if (corrupt_payload)
		slot->payload[0] ^= 0xffU;
	mailmsg_linux_publish(slot, sizeof(*slot));
	slot->commit = message->sequence;
	mailmsg_linux_publish(&slot->commit, sizeof(slot->commit));
	ring->producer = next;
	mailmsg_linux_publish(&ring->producer, sizeof(ring->producer));
	return 0;
}

static int r1_amp_map_mbox_regs(struct device *dev,
				 struct r1_amp_cpu_on_data *data)
{
	struct device_node *mbox_np;
	struct resource res;
	int controller, ret;

	/* Map each distinct controller from the first channel of its DT group. */
	for (controller = 0; controller < R1_AMP_MBOX_CONTROLLERS; controller++) {
		mbox_np = of_parse_phandle(dev->of_node, "mboxes",
			controller * R1_AMP_MBOX_CHANNELS_PER_CONTROLLER);
		if (!mbox_np)
			return dev_err_probe(dev, -EINVAL, "missing mailbox%d phandle\n",
					     controller);

		ret = of_address_to_resource(mbox_np, 0, &res);
		of_node_put(mbox_np);
		if (ret)
			return dev_err_probe(dev, ret, "invalid mailbox%d reg\n", controller);

		data->mbox_regs[controller] =
			devm_ioremap(dev, res.start, resource_size(&res));
		if (!data->mbox_regs[controller])
			return dev_err_probe(dev, -ENOMEM,
					     "failed to map mailbox%d registers\n", controller);
	}

	return 0;
}

static ssize_t image_write(struct file *filp, struct kobject *kobj,
			   struct bin_attribute *attr, char *buf,
			   loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&data->lock);
	if (data->mailmsg_user_removing) {
		ret = -ENODEV;
		goto out;
	}
	if (data->cpu_on_attempted) {
		ret = -EBUSY;
		goto out;
	}
	if (off != data->image_written) {
		ret = -ESPIPE;
		goto out;
	}
	if (count > data->image_size - data->image_written) {
		ret = -EFBIG;
		goto out;
	}

	memcpy(data->carveout + off, buf, count);
	data->image_written += count;

	if (data->image_written == data->image_size) {
		lzamp_dcache_clean(data->carveout, data->image_size);
		flush_icache_range((unsigned long)data->carveout,
				   (unsigned long)data->carveout + data->image_size);
		dsb(sy);
	}
out:
	mutex_unlock(&data->lock);
	return ret;
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct r1_amp_mbox_observation_line *observation;
	struct mailmsg_tx_full_observation *tx_full_observation;
	struct r1_amp_mailmsg_worker_observation_line *worker_observation;
	u64 magic, current_el;
	u32 observation_magic, observation_status, observation_cmd, observation_data;
	u32 tx_full_commit_first, tx_full_commit_second, tx_full_commit_inv;
	u32 tx_full_magic, tx_full_count, tx_full_priority, tx_full_type;
	mailmsg_s32 tx_full_result;
	bool tx_full_valid;
	u32 worker_commit_first, worker_commit_second, worker_commit_inv;
	u32 worker_magic, worker_irq_count, worker_wake_count, worker_drain_count;
	u32 worker_message_count, worker_empty_wake_count, worker_last_pending;
	u32 worker_pending_now, worker_last_priority;
	bool worker_valid;
	u32 a2b_now_status[R1_AMP_MBOX_CONTROLLERS];
	struct r1_amp_mbox_channel *ch;
	int controller, channel;
	ssize_t ret;

	mutex_lock(&data->lock);
	lzamp_dcache_invalidate(r1_amp_status_ptr(data), R1_AMP_CACHE_LINE_SIZE);
	magic = READ_ONCE(((u64 *)r1_amp_status_ptr(data))[0]);
	current_el = READ_ONCE(((u64 *)r1_amp_status_ptr(data))[1]);
	observation = r1_amp_mbox_observation_ptr(data);
	lzamp_dcache_invalidate(observation, sizeof(*observation));
	observation_magic = READ_ONCE(observation->magic);
	observation_status = READ_ONCE(observation->a2b_status);
	observation_cmd = READ_ONCE(observation->command);
	observation_data = READ_ONCE(observation->data);
	tx_full_observation = r1_amp_mailmsg_tx_full_observation_ptr(data);
	lzamp_dcache_invalidate(tx_full_observation, sizeof(*tx_full_observation));
	dsb(sy);
	tx_full_commit_first = READ_ONCE(tx_full_observation->commit);
	tx_full_magic = READ_ONCE(tx_full_observation->magic);
	tx_full_count = READ_ONCE(tx_full_observation->full_count);
	tx_full_priority = READ_ONCE(tx_full_observation->last_priority);
	tx_full_type = READ_ONCE(tx_full_observation->last_type);
	tx_full_result = READ_ONCE(tx_full_observation->last_result);
	lzamp_dcache_invalidate(tx_full_observation, sizeof(*tx_full_observation));
	dsb(sy);
	tx_full_commit_second = READ_ONCE(tx_full_observation->commit);
	tx_full_commit_inv = READ_ONCE(tx_full_observation->commit_inv);
	tx_full_valid = tx_full_magic == MAILMSG_TX_FULL_OBSERVATION_MAGIC &&
		tx_full_commit_first &&
		tx_full_commit_first == tx_full_commit_second &&
		tx_full_commit_inv == ~tx_full_commit_second;
	worker_observation = r1_amp_mailmsg_worker_observation_ptr(data);
	lzamp_dcache_invalidate(worker_observation, sizeof(*worker_observation));
	dsb(sy);
	worker_commit_first = READ_ONCE(worker_observation->commit);
	worker_magic = READ_ONCE(worker_observation->magic);
	worker_irq_count = READ_ONCE(worker_observation->irq_count);
	worker_wake_count = READ_ONCE(worker_observation->wake_count);
	worker_drain_count = READ_ONCE(worker_observation->drain_count);
	worker_message_count = READ_ONCE(worker_observation->message_count);
	worker_empty_wake_count = READ_ONCE(worker_observation->empty_wake_count);
	worker_last_pending = READ_ONCE(worker_observation->last_pending);
	worker_pending_now = READ_ONCE(worker_observation->pending_now);
	worker_last_priority = READ_ONCE(worker_observation->last_priority);
	lzamp_dcache_invalidate(worker_observation, sizeof(*worker_observation));
	dsb(sy);
	worker_commit_second = READ_ONCE(worker_observation->commit);
	worker_commit_inv = READ_ONCE(worker_observation->commit_inv);
	worker_valid =
		worker_magic == R1_AMP_MAILMSG_WORKER_OBSERVATION_MAGIC &&
		worker_commit_first &&
		worker_commit_first == worker_commit_second &&
		worker_commit_inv == ~worker_commit_second;
	for (controller = 0; controller < R1_AMP_MBOX_CONTROLLERS; controller++)
		a2b_now_status[controller] =
			readl_relaxed(data->mbox_regs[controller] +
				      R1_AMP_MBOX_A2B_STATUS);
	ch = data->mbox;
	ret = sysfs_emit(buf,
		"image=%zu/%u affinity=%s (%d) affinity_seen_on=%u mailmsg_state=%s session=%u/%u session_result=%d stop_sequence=%u stop_reply=%u stop_result=%d stop_notify=%d affinity_monitor=%u/%u cpu_on_attempted=%u cpu_on_ret=%d ",
		data->image_written, data->image_size,
		data->affinity_queried ? r1_amp_psci_state_name(data->affinity_state) : "not-queried",
		data->affinity_state, data->affinity_seen_on,
		r1_amp_mailmsg_state_name(data->mailmsg_state),
		data->session_generation, data->peer_generation,
		data->session_result, data->stop_request_sequence,
		data->stop_reply_type, data->stop_result, data->stop_notify_result,
		data->affinity_monitor_count, data->affinity_monitor_error_count,
		data->cpu_on_attempted, data->cpu_on_ret);
	for (controller = 0; controller < R1_AMP_MBOX_CONTROLLERS; controller++) {
		for (channel = 0; channel < R1_AMP_MBOX_CHANNELS_PER_CONTROLLER;
		     channel++) {
			struct r1_amp_mbox_channel *entry =
				&ch[controller * R1_AMP_MBOX_CHANNELS_PER_CONTROLLER + channel];

			ret += sysfs_emit_at(buf, ret,
				"m%dc%d=rx:%d/%#x/%#x,tx:%u/%d,notify:%d/%u/%u/%u ",
				controller, channel, atomic_read(&entry->rx_count),
				READ_ONCE(entry->rx_last_cmd),
				READ_ONCE(entry->rx_last_data), entry->tx_count,
				entry->tx_ret, entry->notify_result,
				entry->notify_sent_count,
				entry->notify_coalesced_count,
				entry->notify_failed_count);
		}
	}
	ret += sysfs_emit_at(buf, ret,
		"a2b_now=m0:%#x mailmsg_notify=%d "
		"notify_inject=%u/%d/%u "
		"mailmsg_tx_full=valid:%u/commit:%u/count:%u/priority:%u/type:%u/result:%d "
		"mailmsg_worker=valid:%u/commit:%u/irq:%u/wake:%u/drain:%u/msg:%u/empty:%u/last:%#x/pending:%#x/priority:%u "
		"mbox_observation=%#x/%#x/%#x/%#x magic=%#llx current_el=%llu\n",
		a2b_now_status[0], data->mailmsg_last_notify_result,
		data->notify_inject_priority, data->notify_inject_errno,
		data->notify_inject_remaining,
		tx_full_valid, tx_full_commit_second, tx_full_count,
		tx_full_priority, tx_full_type, tx_full_result,
		worker_valid, worker_commit_second, worker_irq_count,
		worker_wake_count, worker_drain_count, worker_message_count,
		worker_empty_wake_count, worker_last_pending,
		worker_pending_now, worker_last_priority,
		observation_magic, observation_status,
		observation_cmd, observation_data, magic, current_el);
	mutex_unlock(&data->lock);

	return ret;
}
static DEVICE_ATTR_RO(status);

static ssize_t mailmsg_stats_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	ssize_t ret = 0;
	int priority;

	mutex_lock(&data->lock);
	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		const struct mailmsg_priority_stats *stats =
			&data->mailmsg_endpoint.stats.priority[priority];
		int tx_depth = -1, rx_depth = -1;

		if (data->mailmsg_endpoint.tx && data->mailmsg_endpoint.rx) {
			tx_depth = mailmsg_ring_count(
				&data->mailmsg_endpoint.tx[priority],
				&mailmsg_linux_memory_ops);
			rx_depth = mailmsg_ring_count(
				&data->mailmsg_endpoint.rx[priority],
				&mailmsg_linux_memory_ops);
		}
		ret += sysfs_emit_at(buf, ret,
			"p%d tx=%u full=%u high=%u depth=%d notify=%u/%u/%u rx=%u incomplete=%u crc=%u invalid=%u stale=%u depth=%d\n",
			priority, stats->tx_enqueued, stats->tx_full,
			stats->tx_high_water, tx_depth, stats->notify_sent,
			stats->notify_coalesced, stats->notify_failed, stats->rx_ok,
			stats->rx_incomplete, stats->rx_bad_crc, stats->rx_invalid,
			stats->rx_stale,
			rx_depth);
	}
	mutex_unlock(&data->lock);
	return ret;
}
static DEVICE_ATTR_RO(mailmsg_stats);

/* Unlike status, this reads PSCI_AFFINITY_INFO for every sysfs read. */
static ssize_t affinity_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	int state;

	if (!psci_ops.affinity_info)
		return -EOPNOTSUPP;

	mutex_lock(&data->lock);
	state = r1_amp_refresh_affinity_locked(data);
	mutex_unlock(&data->lock);

	return sysfs_emit(buf, "mpidr=%#llx level=0 state=%s (%d)\n",
			  data->mpidr, r1_amp_psci_state_name(state), state);
}
static DEVICE_ATTR_RO(affinity_state);

/* Caller holds data->lock.  All A2B writers in this driver use this helper,
 * so a legacy diagnostic request cannot race a MailMsg doorbell. */
static int r1_amp_mbox_direct_send_locked(struct r1_amp_cpu_on_data *data,
					  struct r1_amp_mbox_channel *channel,
					  u32 command, u32 value)
{
	u32 status;

	status = readl_relaxed(data->mbox_regs[channel->controller] +
			       R1_AMP_MBOX_A2B_STATUS);
	if (status & BIT(channel->controller_channel)) {
		channel->tx_ret = -EBUSY;
		return -EBUSY;
	}

	channel->tx_msg.cmd = command;
	channel->tx_msg.data = value;
	/* Publish shared-memory data before the final DAT doorbell write. */
	wmb();
	writel_relaxed(command, data->mbox_regs[channel->controller] +
		       R1_AMP_MBOX_A2B_CMD(channel->controller_channel));
	writel_relaxed(value, data->mbox_regs[channel->controller] +
		       R1_AMP_MBOX_A2B_DAT(channel->controller_channel));
	channel->tx_ret = 0;
	channel->tx_count++;
	return 0;
}

static ssize_t ping_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct r1_amp_payload_line *payload;
	struct r1_amp_commit_line *commit;
	struct r1_amp_mbox_observation_line *observation;
	u32 value;
	u64 sequence;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (ret)
		return ret;

	mutex_lock(&data->lock);
	/* This legacy one-shot command does not wake the MailMsg worker.  Once
	 * the lifecycle exists it must not share ch3 with protocol doorbells. */
	if (data->mailmsg_state != R1_AMP_MAILMSG_UNARMED) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret)
		goto out;

	sequence = ++data->next_sequence;
	if (!sequence)
		sequence = ++data->next_sequence;

	payload = r1_amp_protocol_ptr(data, R1_AMP_REQ_PAYLOAD_OFF);
	commit = r1_amp_protocol_ptr(data, R1_AMP_REQ_COMMIT_OFF);
	memset(payload, 0, sizeof(*payload));
	WRITE_ONCE(payload->command, R1_AMP_CMD_PING);
	WRITE_ONCE(payload->value, value);
	lzamp_dcache_clean(payload, sizeof(*payload));
	dsb(sy);

	WRITE_ONCE(commit->sequence, sequence);
	WRITE_ONCE(commit->sequence_inv, ~sequence);
	lzamp_dcache_clean(commit, sizeof(*commit));
	dsb(sy);

	/* A fresh observation is required for each one-shot TX3 probe. */
	observation = r1_amp_mbox_observation_ptr(data);
	memset(observation, 0, sizeof(*observation));
	lzamp_dcache_clean(observation, sizeof(*observation));
	dsb(sy);

	ret = r1_amp_mbox_direct_send_locked(
		data, &data->mbox[R1_AMP_MBOX_TX_CHANNEL], R1_AMP_CMD_PING,
		value);
	if (ret < 0)
		goto out;
	data->mbox_a2b_at_tx_status =
		readl_relaxed(data->mbox_regs[0] + R1_AMP_MBOX_A2B_STATUS);
	data->mbox_a2b_at_tx_cmd = readl_relaxed(data->mbox_regs[0] +
				       R1_AMP_MBOX_A2B_CMD(R1_AMP_MBOX_TX_CHANNEL));
	data->mbox_a2b_at_tx_data = readl_relaxed(data->mbox_regs[0] +
				        R1_AMP_MBOX_A2B_DAT(R1_AMP_MBOX_TX_CHANNEL));
	ret = count;
out:
	mutex_unlock(&data->lock);
	return ret;
}
static DEVICE_ATTR_WO(ping);

/*
 * Matrix preflight doorbell.  Unlike ping, this has no shared-memory
 * payload: it exercises exactly one requested V1 channel and relies on the
 * Zephyr ISR to reply on the same-numbered B2A channel.
 */
static ssize_t doorbell_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct r1_amp_mbox_channel *channel;
	u32 index, value;
	int ret;

	if (sscanf(buf, "%u %u", &index, &value) != 2)
		return -EINVAL;
	if (index >= R1_AMP_MBOX_CHANNELS)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret)
		goto out;

	channel = &data->mbox[index];
	ret = r1_amp_mbox_direct_send_locked(data, channel,
					     0xa2b00000U | index, value);
	if (ret < 0)
		goto out;
	ret = count;
out:
	mutex_unlock(&data->lock);
	return ret;
}
static DEVICE_ATTR_WO(doorbell);

static int mailmsg_ring_result_to_errno(int result)
{
	switch (result) {
	case MAILMSG_RING_OK:
		return 0;
	case MAILMSG_RING_EMPTY:
		return -ENODATA;
	case MAILMSG_RING_FULL:
		return -ENOSPC;
	case MAILMSG_RING_INCOMPLETE:
		return -EAGAIN;
	case MAILMSG_RING_BAD_CRC:
		return -EBADMSG;
	case MAILMSG_RING_INVALID:
		return -EPROTO;
	case MAILMSG_ENDPOINT_INVALID:
		return -EINVAL;
	case MAILMSG_ENDPOINT_PROTOCOL_MISMATCH:
		return -EPROTO;
	case MAILMSG_ENDPOINT_STALE_SESSION:
	case MAILMSG_ENDPOINT_STALE_FRAME:
		return -ESTALE;
	default:
		return -EIO;
	}
}

static const char * const r1_amp_mailmsg_user_names[MAILMSG_PRIORITY_COUNT] = {
	"mailmsg-p0", "mailmsg-p1", "mailmsg-p2", "mailmsg-p3",
};

static int r1_amp_mailmsg_user_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct r1_amp_mailmsg_user_channel *channel;
	struct r1_amp_cpu_on_data *data;
	struct r1_amp_mailmsg_user_file *user_file;
	int ret;

	user_file = kzalloc(sizeof(*user_file), GFP_KERNEL);
	if (!user_file)
		return -ENOMEM;

	channel = container_of(miscdev, struct r1_amp_mailmsg_user_channel,
			       miscdev);
	data = channel->parent;
	mutex_lock(&data->lock);
	if (data->mailmsg_user_removing) {
		mutex_unlock(&data->lock);
		kfree(user_file);
		return -ENODEV;
	}
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret) {
		mutex_unlock(&data->lock);
		kfree(user_file);
		return ret;
	}
	atomic_inc(&data->mailmsg_user_open_count);
	mutex_unlock(&data->lock);
	user_file->channel = channel;
	file->private_data = user_file;
	return nonseekable_open(inode, file);
}

static int r1_amp_mailmsg_user_release(struct inode *inode, struct file *file)
{
	struct r1_amp_mailmsg_user_file *user_file = file->private_data;
	struct r1_amp_mailmsg_user_channel *channel = user_file->channel;
	struct r1_amp_cpu_on_data *data = channel->parent;

	mutex_lock(&data->lock);
	if (user_file->reader_claimed)
		atomic_dec(&channel->reader_open_count);
	atomic_dec(&data->mailmsg_user_open_count);
	mutex_unlock(&data->lock);
	kfree(user_file);
	return 0;
}

static int r1_amp_mailmsg_user_claim_reader(
		struct r1_amp_mailmsg_user_file *user_file)
{
	struct r1_amp_mailmsg_user_channel *channel = user_file->channel;
	struct r1_amp_cpu_on_data *data = channel->parent;
	int ret = 0;

	mutex_lock(&data->lock);
	if (!user_file->reader_claimed) {
		if (atomic_read(&channel->reader_open_count)) {
			ret = -EBUSY;
		} else {
			atomic_inc(&channel->reader_open_count);
			user_file->reader_claimed = true;
		}
	}
	mutex_unlock(&data->lock);

	return ret;
}

static ssize_t r1_amp_mailmsg_user_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct r1_amp_mailmsg_user_file *user_file = file->private_data;
	struct r1_amp_mailmsg_user_channel *channel = user_file->channel;
	struct r1_amp_cpu_on_data *data = channel->parent;
	struct mailmsg_user_frame frame = { };
	struct mailmsg_message message;
	int generation;
	int ret;

	if (count != sizeof(frame))
		return -EINVAL;
	ret = r1_amp_mailmsg_user_claim_reader(user_file);
	if (ret)
		return ret;

	for (;;) {
		generation = atomic_read(&channel->rx_generation);
		mutex_lock(&data->lock);
		ret = r1_amp_mailmsg_data_gate_locked(data);
		if (!ret) {
			ret = mailmsg_endpoint_receive(&data->mailmsg_endpoint,
					(enum mailmsg_priority)channel->priority,
					&message);
		}
		mutex_unlock(&data->lock);

		if (!ret) {
			frame.priority = channel->priority;
			frame.type = message.type;
			frame.sequence = message.sequence;
			frame.length = message.length;
			memcpy(frame.payload, message.payload, message.length);
			if (copy_to_user(buf, &frame, sizeof(frame)))
				return -EFAULT;
			return sizeof(frame);
		}
		if (ret == -EAGAIN)
			return ret;
		if (ret == -ENOLINK || ret == -ESHUTDOWN)
			return ret;
		if (ret != MAILMSG_RING_EMPTY)
			return mailmsg_ring_result_to_errno(ret);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(channel->rx_wait,
			atomic_read(&channel->rx_generation) != generation);
		if (ret)
			return ret;
	}
}

static ssize_t r1_amp_mailmsg_user_write(struct file *file,
					 const char __user *buf, size_t count,
					 loff_t *ppos)
{
	struct r1_amp_mailmsg_user_file *user_file = file->private_data;
	struct r1_amp_mailmsg_user_channel *channel = user_file->channel;
	struct r1_amp_cpu_on_data *data = channel->parent;
	struct mailmsg_user_frame frame;
	struct mailmsg_send_result send_result = { };
	int ret;

	if (count != sizeof(frame))
		return -EINVAL;
	if (copy_from_user(&frame, buf, sizeof(frame)))
		return -EFAULT;
	if (frame.priority != channel->priority ||
	    frame.length > MAILMSG_PAYLOAD_BYTES)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (!ret) {
		ret = mailmsg_endpoint_send(&data->mailmsg_endpoint,
					(enum mailmsg_priority)channel->priority,
					frame.type, frame.payload, frame.length,
					&send_result);
	}
	mutex_unlock(&data->lock);
	if (ret == -EAGAIN || ret == -ENOLINK || ret == -ESHUTDOWN)
		return ret;

	if (ret) {
		if (ret <= MAILMSG_ENDPOINT_INVALID)
			return mailmsg_ring_result_to_errno(ret);
		return mailmsg_ring_result_to_errno(send_result.queue_result);
	}

	return sizeof(frame);
}

static __poll_t r1_amp_mailmsg_user_poll(struct file *file,
					 struct poll_table_struct *wait)
{
	struct r1_amp_mailmsg_user_file *user_file = file->private_data;
	struct r1_amp_mailmsg_user_channel *channel = user_file->channel;
	struct r1_amp_cpu_on_data *data = channel->parent;
	__poll_t mask = 0;

	if (r1_amp_mailmsg_user_claim_reader(user_file))
		return EPOLLERR;

	poll_wait(file, &channel->rx_wait, wait);
	mutex_lock(&data->lock);
	if (data->mailmsg_state == R1_AMP_MAILMSG_OFFLINE ||
	    data->mailmsg_state == R1_AMP_MAILMSG_STOPPING) {
		mask = EPOLLHUP | EPOLLERR;
	} else if (data->mailmsg_state == R1_AMP_MAILMSG_STARTING) {
		mask = EPOLLERR;
	} else if (data->mailmsg_state == R1_AMP_MAILMSG_ACTIVE &&
		    mailmsg_endpoint_has_received(&data->mailmsg_endpoint,
					  (enum mailmsg_priority)channel->priority) > 0)
		mask = EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&data->lock);

	return mask;
}

static const struct file_operations r1_amp_mailmsg_user_fops = {
	.owner = THIS_MODULE,
	.open = r1_amp_mailmsg_user_open,
	.release = r1_amp_mailmsg_user_release,
	.read = r1_amp_mailmsg_user_read,
	.write = r1_amp_mailmsg_user_write,
	.poll = r1_amp_mailmsg_user_poll,
	.llseek = noop_llseek,
};

static void r1_amp_unregister_mailmsg_user(struct r1_amp_cpu_on_data *data)
{
	int priority;

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		if (data->mailmsg_user[priority].registered) {
			misc_deregister(&data->mailmsg_user[priority].miscdev);
			data->mailmsg_user[priority].registered = false;
		}
	}
}

static int r1_amp_register_mailmsg_user(struct device *dev,
					 struct r1_amp_cpu_on_data *data)
{
	int priority;
	int ret;

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		struct r1_amp_mailmsg_user_channel *channel =
			&data->mailmsg_user[priority];

		channel->miscdev.minor = MISC_DYNAMIC_MINOR;
		channel->miscdev.name = r1_amp_mailmsg_user_names[priority];
		channel->miscdev.fops = &r1_amp_mailmsg_user_fops;
		channel->miscdev.parent = dev;
		channel->miscdev.mode = 0600;
		ret = misc_register(&channel->miscdev);
		if (ret) {
			r1_amp_unregister_mailmsg_user(data);
			return dev_err_probe(dev, ret,
				"failed to register MailMsg priority %d\n", priority);
		}
		channel->registered = true;
	}

	return 0;
}

/* Test-only direct enqueue for CRC corruption and queue-without-doorbell. */
static int mailmsg_test_enqueue_locked(struct r1_amp_cpu_on_data *data,
				       u32 priority, u32 value,
				       bool corrupt_payload)
{
	struct mailmsg_shared *shared;
	struct mailmsg_message request = { };

	shared = mailmsg_linux_shared_ptr(data);
	mailmsg_linux_acquire(shared, sizeof(*shared));
	if (shared->magic != MAILMSG_PROTOCOL_MAGIC ||
	    shared->version != MAILMSG_PROTOCOL_VERSION ||
	    shared->generation != data->mailmsg_endpoint.generation) {
		return -EPROTO;
	}

	request.type = MAILMSG_MSG_PING;
	request.generation = data->mailmsg_endpoint.generation;
	request.sequence = ++data->mailmsg_endpoint.next_sequence;
	if (!request.sequence)
		request.sequence = ++data->mailmsg_endpoint.next_sequence;
	request.length = sizeof(value);
	mailmsg_store_payload_u32(request.payload, value);
	return mailmsg_linux_ring_push(&shared->linux_to_cpu3[priority], &request,
				       corrupt_payload);
}

/* Caller holds data->lock.  MailMsg payload lives in shared memory; the
 * mailbox carries only a level-like wakeup hint.  Keep at most one hardware
 * doorbell pending per priority.  Do not route this high-rate hint through
 * the generic mailbox TX queue: its polled TX completion can lag behind the
 * remote A2B clear and accumulate redundant software entries. */
static int mailmsg_notify_locked(struct r1_amp_cpu_on_data *data, u32 priority)
{
	struct r1_amp_mbox_channel *channel = &data->mbox[priority];
	int result;

	if (!data->lifecycle_notify_bypass && data->notify_inject_remaining &&
	    data->notify_inject_priority == priority) {
		data->notify_inject_remaining--;
		result = data->notify_inject_errno;
		channel->tx_ret = result;
		channel->notify_failed_count++;
		channel->notify_result = result;
		data->mailmsg_last_notify_result = result;
		return result;
	}

	result = r1_amp_mbox_direct_send_locked(
		data, channel, R1_AMP_MAILMSG_DOORBELL | priority, priority);
	if (result == -EBUSY) {
		/* The pending doorbell will make CPU3 drain this priority ring. */
		result = MAILMSG_NOTIFY_COALESCED;
		channel->notify_coalesced_count++;
	} else if (result < 0) {
		channel->notify_failed_count++;
	} else {
		result = MAILMSG_NOTIFY_SENT;
		channel->notify_sent_count++;
	}

	channel->notify_result = result;
	data->mailmsg_last_notify_result = result;
	return result;
}

static int mailmsg_linux_notify(void *context,
				enum mailmsg_priority priority)
{
	return mailmsg_notify_locked(context, priority);
}

static const struct mailmsg_notify_ops mailmsg_linux_notify_ops = {
	.notify = mailmsg_linux_notify,
};

/*
 * MailMsg test entry point.  Input is "priority value".  The message is put
 * into the priority's Linux->CPU3 SPSC ring before its matching doorbell is
 * sent.  The mailbox carries only a wake-up/priority hint, never payload.
 */
static ssize_t mailmsg_send_ping(struct device *dev, const char *buf,
				 size_t count, bool corrupt_payload)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct mailmsg_send_result send_result = { };
	mailmsg_u8 payload[sizeof(u32)];
	u32 priority, value;
	int ret;

	if (sscanf(buf, "%u %u", &priority, &value) != 2 ||
	    priority >= MAILMSG_PRIORITY_COUNT)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret)
		goto out;

	if (corrupt_payload) {
		ret = mailmsg_test_enqueue_locked(data, priority, value, true);
		if (ret)
			goto out;
		ret = mailmsg_notify_locked(data, priority);
	} else {
		mailmsg_store_payload_u32(payload, value);
		ret = mailmsg_endpoint_send(&data->mailmsg_endpoint,
					    (enum mailmsg_priority)priority,
					    MAILMSG_MSG_PING, payload,
					    sizeof(payload), &send_result);
		if (ret) {
			if (ret <= MAILMSG_ENDPOINT_INVALID)
				ret = mailmsg_ring_result_to_errno(ret);
			else
				ret = mailmsg_ring_result_to_errno(
					send_result.queue_result);
			goto out;
		}
		ret = send_result.notify_result;
	}
	/* The sysfs write reports data-plane admission.  Notification outcome is
	 * independently observable in status; a future upper API may retry or
	 * degrade on a negative result, but this prototype never rolls back a
	 * committed frame. */
	if (!mailmsg_notify_accepted(ret) && ret < 0)
		dev_warn(dev, "mailmsg priority %u queued but notify failed: %d\n",
			 priority, ret);
	ret = count;
out:
	mutex_unlock(&data->lock);
	return ret;
}

static ssize_t mailmsg_ping_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	return mailmsg_send_ping(dev, buf, count, false);
}
static DEVICE_ATTR_WO(mailmsg_ping);

/* A controlled stop begins only after user space has drained and closed every
 * priority device.  The kernel then owns p0 solely to observe STOP_READY or
 * STOP_REFUSED; existing queued business frames are never discarded to make
 * room for lifecycle control. */
static int r1_amp_mailmsg_stop_preflight_locked(
		struct r1_amp_cpu_on_data *data)
{
	struct mailmsg_shared *shared = mailmsg_linux_shared_ptr(data);
	int priority;
	int ret;

	if (atomic_read(&data->mailmsg_user_open_count))
		return -EBUSY;
	if (!data->mailmsg_endpoint.tx || !data->mailmsg_endpoint.rx)
		return -EPROTO;

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		ret = mailmsg_endpoint_has_received(&data->mailmsg_endpoint,
						    priority);
		if (ret != MAILMSG_RING_EMPTY)
			return ret > 0 ? -EBUSY : mailmsg_ring_result_to_errno(ret);
		ret = mailmsg_ring_has_data(&shared->linux_to_cpu3[priority],
					    &mailmsg_linux_memory_ops);
		if (ret != MAILMSG_RING_EMPTY)
			return ret > 0 ? -EBUSY : mailmsg_ring_result_to_errno(ret);
	}

	/* Do not merge the lifecycle request into an earlier unobserved doorbell. */
	if (readl_relaxed(data->mbox_regs[0] + R1_AMP_MBOX_A2B_STATUS) & 0xfU)
		return -EBUSY;

	return 0;
}

/* Root-only lifecycle entry point.  This does not wait for CPU_OFF: a sysfs
 * store must not hold data->lock across the peer's response.  Status exposes
 * STOP_READY/REFUSED progress; the affinity worker remains the final OFF
 * authority. */
static ssize_t mailmsg_stop_store(struct device *dev,
				  struct device_attribute *attr, const char *buf,
				  size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct mailmsg_send_result send_result = { };
	int ret;

	if (!sysfs_streq(buf, "stop"))
		return -EINVAL;

	mutex_lock(&data->lock);
	if (data->mailmsg_state == R1_AMP_MAILMSG_OFFLINE) {
		ret = -ENOLINK;
		goto out;
	}
	if (data->mailmsg_state != R1_AMP_MAILMSG_ACTIVE) {
		ret = -EBUSY;
		goto out;
	}
	ret = r1_amp_mailmsg_stop_preflight_locked(data);
	if (ret)
		goto out;

	data->mailmsg_state = R1_AMP_MAILMSG_STOPPING;
	data->stop_request_sequence = 0;
	data->stop_reply_type = 0;
	data->stop_result = -EINPROGRESS;
	data->stop_notify_result = -EINPROGRESS;
	/* Fault injection validates the business notification adapter.  It must
	 * never strand a lifecycle request in STOPPING. */
	data->lifecycle_notify_bypass = true;
	ret = mailmsg_endpoint_send(&data->mailmsg_endpoint,
				    MAILMSG_PRIO_CRITICAL,
				    MAILMSG_MSG_STOP_REQUEST, NULL, 0, &send_result);
	data->lifecycle_notify_bypass = false;
	if (ret) {
		/* No frame was committed when endpoint_send() reports a queue error. */
		data->mailmsg_state = R1_AMP_MAILMSG_ACTIVE;
		if (ret <= MAILMSG_ENDPOINT_INVALID)
			data->stop_result = mailmsg_ring_result_to_errno(ret);
		else
			data->stop_result = mailmsg_ring_result_to_errno(
				send_result.queue_result);
		ret = data->stop_result;
		goto out;
	}

	data->stop_request_sequence = send_result.sequence;
	data->stop_notify_result = send_result.notify_result;
	data->lifecycle_deadline = jiffies +
		msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS);
	mod_delayed_work(system_wq, &data->lifecycle_timeout_work,
			 msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS));
	/* Queue admission is durable; retain STOPPING if the doorbell failed so
	 * later business writes cannot race a request that CPU3 may still observe. */
	ret = send_result.notify_result;
	if (!mailmsg_notify_accepted(ret) && ret < 0)
		dev_warn(dev, "MailMsg STOP_REQUEST queued but notify failed: %d\n", ret);
	else
		ret = 0;
out:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(mailmsg_stop);

/*
 * Test-only entry point.  Input matches mailmsg_ping ("priority value"),
 * but one payload byte is changed after CRC generation.  The frame keeps a
 * valid commit so the remote side must reject it specifically as BAD_CRC.
 */
static ssize_t mailmsg_crc_inject_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	return mailmsg_send_ping(dev, buf, count, true);
}
static DEVICE_ATTR_WO(mailmsg_crc_inject);

/* Test-only notification failure injection.  A committed frame is retained;
 * only the selected next N doorbells return the requested negative errno. */
static ssize_t mailmsg_notify_inject_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	u32 priority, remaining;
	int injected_errno;
	int ret = 0;

	mutex_lock(&data->lock);
	if (sysfs_streq(buf, "clear")) {
		data->notify_inject_remaining = 0;
		goto out;
	}
	if (sscanf(buf, "%u %d %u", &priority, &injected_errno, &remaining) != 3 ||
	    priority >= MAILMSG_PRIORITY_COUNT || injected_errno >= 0 ||
	    injected_errno < -MAX_ERRNO || !remaining || remaining > 1000) {
		ret = -EINVAL;
		goto out;
	}
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret)
		goto out;
	data->notify_inject_priority = priority;
	data->notify_inject_errno = injected_errno;
	data->notify_inject_remaining = remaining;
out:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(mailmsg_notify_inject);

/* Test-only queue boundary hook.  It intentionally omits a doorbell and may
 * run before CPU3 starts, so a test can observe the ring's immediate -ENOSPC
 * result without mailbox state or remote scheduling influencing the result. */
static ssize_t mailmsg_queue_push_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	u32 priority, value;
	int ret;

	if (sscanf(buf, "%u %u", &priority, &value) != 2 ||
	    priority >= MAILMSG_PRIORITY_COUNT)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (!ret)
		ret = mailmsg_test_enqueue_locked(data, priority, value, false);
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(mailmsg_queue_push);

	/* Drain independent reverse rings.  Feedback frames carry the original
 * sequence in payload[0..3], followed by ACK status or NACK reason. */
static ssize_t mailmsg_response_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct mailmsg_message response;
	int priority;
	ssize_t ret = 0;

	mutex_lock(&data->lock);
	if (!data->mailmsg_endpoint.rx) {
		ret = sysfs_emit(buf, "valid=0 reason=protocol-not-ready\n");
		goto out;
	}
	ret = r1_amp_mailmsg_data_gate_locked(data);
	if (ret) {
		const char *reason;

		switch (data->mailmsg_state) {
		case R1_AMP_MAILMSG_STARTING:
		case R1_AMP_MAILMSG_STOPPING:
			reason = "lifecycle-pending";
			break;
		case R1_AMP_MAILMSG_START_TIMEOUT:
		case R1_AMP_MAILMSG_STOP_TIMEOUT:
			reason = "lifecycle-timeout";
			break;
		case R1_AMP_MAILMSG_OFFLINE:
			reason = "offline";
			break;
		default:
			reason = "unarmed";
			break;
		}
		ret = sysfs_emit(buf, "valid=0 reason=%s\n", reason);
		goto out;
	}

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		int pop_ret;

		while ((pop_ret = mailmsg_endpoint_receive(
			       &data->mailmsg_endpoint,
			       (enum mailmsg_priority)priority, &response)) !=
		       MAILMSG_RING_EMPTY) {
			if (pop_ret) {
				ret += sysfs_emit_at(buf, ret,
					"priority=%d valid=0 ret=%d\n", priority, pop_ret);
				if (pop_ret == MAILMSG_RING_INCOMPLETE ||
				    pop_ret <= MAILMSG_ENDPOINT_INVALID)
					break;
				continue;
			}
			if ((response.type == MAILMSG_MSG_ACK ||
			     response.type == MAILMSG_MSG_NACK) &&
			    response.length >= MAILMSG_FEEDBACK_BYTES) {
				ret += sysfs_emit_at(buf, ret,
					"priority=%d valid=1 type=%u sequence=%u peer_sequence=%u status=%d\n",
					priority, response.type, response.sequence,
					mailmsg_payload_u32(
						&response.payload[MAILMSG_FEEDBACK_SEQUENCE_OFFSET]),
					(s32)mailmsg_payload_u32(
						&response.payload[MAILMSG_FEEDBACK_STATUS_OFFSET]));
			} else {
				ret += sysfs_emit_at(buf, ret,
					"priority=%d valid=1 type=%u sequence=%u value=%u\n",
					priority, response.type, response.sequence,
					response.length >= sizeof(u32) ?
					mailmsg_payload_u32(response.payload) : 0);
			}
		}
	}
	if (!ret)
		ret = sysfs_emit(buf, "valid=0 reason=empty\n");
out:
	mutex_unlock(&data->lock);
	return ret;
}
static DEVICE_ATTR_RO(mailmsg_response);

static ssize_t response_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	struct r1_amp_payload_line *payload;
	struct r1_amp_commit_line *commit;
	u64 sequence, sequence_inv;
	u32 command = 0, value = 0, result = 0;
	s32 status = 0;
	bool valid;
	int ret;

	mutex_lock(&data->lock);
	commit = r1_amp_protocol_ptr(data, R1_AMP_RSP_COMMIT_OFF);
	lzamp_dcache_invalidate(commit, sizeof(*commit));
	sequence = READ_ONCE(commit->sequence);
	sequence_inv = READ_ONCE(commit->sequence_inv);
	valid = sequence && sequence_inv == ~sequence;

	if (valid) {
		payload = r1_amp_protocol_ptr(data, R1_AMP_RSP_PAYLOAD_OFF);
		lzamp_dcache_invalidate(payload, sizeof(*payload));
		command = READ_ONCE(payload->command);
		value = READ_ONCE(payload->value);
		status = READ_ONCE(payload->status);
		result = READ_ONCE(payload->result);
	}

	ret = sysfs_emit(buf,
		"request_seq=%llu response_seq=%llu valid=%u command=%u value=%u status=%d result=%u\n",
		data->next_sequence, sequence, valid, command, value, status,
		result);
	mutex_unlock(&data->lock);
	return ret;
}
static DEVICE_ATTR_RO(response);

static ssize_t start_store(struct device *dev,
			   struct device_attribute *attr, const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "start"))
		return -EINVAL;

	mutex_lock(&data->lock);
	if (data->mailmsg_user_removing) {
		ret = -ENODEV;
		goto out;
	}
	if (data->cpu_on_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (data->image_written != data->image_size) {
		ret = -ENODATA;
		goto out;
	}
	if (!psci_ops.affinity_info || !psci_ops.cpu_on) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	r1_amp_refresh_affinity_locked(data);
	if (data->affinity_state != PSCI_0_2_AFFINITY_LEVEL_OFF) {
		ret = -EBUSY;
		goto out;
	}

	/* Legacy control and diagnostics occupy the first 0x200 bytes.  MailMsg
	 * starts at 0x200 and is initialized before CPU3 sees a doorbell. */
	data->session_generation++;
	if (!data->session_generation)
		data->session_generation++;
	memset(r1_amp_status_ptr(data), 0, R1_AMP_STATE_SIZE);
	mailmsg_linux_shared_ptr(data)->magic = MAILMSG_PROTOCOL_MAGIC;
	mailmsg_linux_shared_ptr(data)->version = MAILMSG_PROTOCOL_VERSION;
	mailmsg_linux_shared_ptr(data)->generation = data->session_generation;
	lzamp_dcache_clean(r1_amp_status_ptr(data), R1_AMP_STATE_SIZE);
	dsb(sy);
	ret = mailmsg_endpoint_bind(&data->mailmsg_endpoint,
				    mailmsg_linux_shared_ptr(data),
				    &mailmsg_linux_memory_ops,
				    &data->mailmsg_notify_endpoint,
				    MAILMSG_ENDPOINT_LINUX);
	if (ret) {
		ret = -EPROTO;
		goto out;
	}

	data->peer_generation = 0;
	data->session_result = -EINPROGRESS;
	data->stop_request_sequence = 0;
	data->stop_reply_type = 0;
	data->stop_result = -EAGAIN;
	data->stop_notify_result = -EAGAIN;
	data->mailmsg_state = R1_AMP_MAILMSG_STARTING;
	data->lifecycle_notify_bypass = false;
	data->affinity_seen_on = false;
	data->cpu_on_attempted = true;
	data->cpu_on_ret = psci_ops.cpu_on(data->mpidr, data->entry);
	if (data->cpu_on_ret) {
		data->mailmsg_state = R1_AMP_MAILMSG_UNARMED;
		data->session_result = data->cpu_on_ret;
	} else {
		data->lifecycle_deadline = jiffies +
			msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS);
		mod_delayed_work(system_wq, &data->affinity_work,
				 msecs_to_jiffies(R1_AMP_AFFINITY_MONITOR_MS));
		mod_delayed_work(system_wq, &data->lifecycle_timeout_work,
				 msecs_to_jiffies(R1_AMP_LIFECYCLE_TIMEOUT_MS));
	}
	ret = data->cpu_on_ret;
	out:
	mutex_unlock(&data->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(start);

/* End one stopped CPU3 session so a new image may be loaded.  This is a
 * destructive session boundary, not a retry: old shared-ring frames are
 * discarded by the next start() initialization. */
static ssize_t rearm_store(struct device *dev,
			   struct device_attribute *attr, const char *buf, size_t count)
{
	struct r1_amp_cpu_on_data *data = dev_get_drvdata(dev);
	int controller;
	int ret = 0;

	if (!sysfs_streq(buf, "rearm"))
		return -EINVAL;

	mutex_lock(&data->lock);
	if (!data->cpu_on_attempted) {
		ret = -EINVAL;
		goto out;
	}
	if (atomic_read(&data->mailmsg_user_open_count)) {
		ret = -EBUSY;
		goto out;
	}
	if (!psci_ops.affinity_info) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	r1_amp_refresh_affinity_locked(data);
	if (data->affinity_state != PSCI_0_2_AFFINITY_LEVEL_OFF) {
		ret = -EBUSY;
		goto out;
	}
	for (controller = 0; controller < R1_AMP_MBOX_CONTROLLERS; controller++) {
		if (readl_relaxed(data->mbox_regs[controller] +
				  R1_AMP_MBOX_A2B_STATUS)) {
			ret = -EBUSY;
			goto out;
		}
	}

	memset(&data->mailmsg_endpoint, 0, sizeof(data->mailmsg_endpoint));
	cancel_delayed_work(&data->lifecycle_timeout_work);
	data->image_written = 0;
	data->cpu_on_attempted = false;
	data->cpu_on_ret = -EAGAIN;
	data->affinity_seen_on = false;
	data->mailmsg_state = R1_AMP_MAILMSG_UNARMED;
	data->peer_generation = 0;
	data->session_result = -EAGAIN;
	data->next_sequence = 0;
	data->mailmsg_last_notify_result = -EAGAIN;
	data->stop_request_sequence = 0;
	data->stop_reply_type = 0;
	data->stop_result = -EAGAIN;
	data->stop_notify_result = -EAGAIN;
	data->notify_inject_remaining = 0;
	data->lifecycle_notify_bypass = false;
out:
	mutex_unlock(&data->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(rearm);

static void r1_amp_free_mbox_channels(struct r1_amp_cpu_on_data *data)
{
	int i;

	for (i = 0; i < R1_AMP_MBOX_CHANNELS; i++) {
		if (!IS_ERR_OR_NULL(data->mbox[i].chan)) {
			mbox_free_channel(data->mbox[i].chan);
			data->mbox[i].chan = NULL;
		}
	}
}

static int r1_amp_request_mbox_channels(struct device *dev,
					 struct r1_amp_cpu_on_data *data)
{
	static const char * const names[R1_AMP_MBOX_CHANNELS] = {
		"m0ch0", "m0ch1", "m0ch2", "m0ch3",
	};
	int i, ret;

	for (i = 0; i < R1_AMP_MBOX_CHANNELS; i++) {
		struct r1_amp_mbox_channel *channel = &data->mbox[i];

		channel->parent = data;
		channel->index = i;
		channel->controller = i / R1_AMP_MBOX_CHANNELS_PER_CONTROLLER;
		channel->controller_channel =
			i % R1_AMP_MBOX_CHANNELS_PER_CONTROLLER;
		channel->client.dev = dev;
		/*
		 * The Rockchip controller exposes A2B_STATUS through last_tx_done().
		 * Do not claim client-managed ACK completion here: this client does
		 * not call mbox_client_txdone(), and doing so would leave later
		 * doorbells queued forever.
		 */
		channel->client.knows_txdone = false;
		channel->client.rx_callback = r1_amp_mbox_rx_callback;
		atomic_set(&channel->rx_count, 0);
		channel->tx_ret = -EAGAIN;
		channel->notify_result = -EAGAIN;
		channel->chan = mbox_request_channel_byname(&channel->client,
							    names[i]);
		if (IS_ERR(channel->chan)) {
			ret = PTR_ERR(channel->chan);
			channel->chan = NULL;
			r1_amp_free_mbox_channels(data);
			return dev_err_probe(dev, ret,
					     "failed to request %s mailbox channel\n",
					     names[i]);
		}
	}
	data->mailmsg_last_notify_result = -EAGAIN;

	return 0;
}

static int r1_amp_cpu_on_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *memory_np;
	struct reserved_mem *rmem;
	struct r1_amp_cpu_on_data *data;
	u32 image_size, status_offset;
	u64 entry, mpidr;
	int priority;
	int ret;

	if (of_property_read_u64(dev->of_node, "target-mpidr", &mpidr) ||
	    of_property_read_u64(dev->of_node, "zephyr-entry", &entry) ||
	    of_property_read_u32(dev->of_node, "zephyr-image-size", &image_size) ||
	    of_property_read_u32(dev->of_node, "status-offset", &status_offset))
		return dev_err_probe(dev, -EINVAL, "missing launch geometry\n");

	if (mpidr != R1_AMP_A55_CORE3_MPIDR)
		return dev_err_probe(dev, -EINVAL, "refusing MPIDR %#llx\n", mpidr);

	memory_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!memory_np)
		return dev_err_probe(dev, -EINVAL, "missing memory-region\n");
	rmem = of_reserved_mem_lookup(memory_np);
	of_node_put(memory_np);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "reserved memory unavailable\n");

	if (!image_size || image_size > status_offset ||
	    status_offset > rmem->size - R1_AMP_STATE_SIZE ||
	    entry < rmem->base || entry >= rmem->base + rmem->size)
		return dev_err_probe(dev, -EINVAL, "invalid launch geometry\n");

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->carveout = devm_memremap(dev, rmem->base, rmem->size, MEMREMAP_WB);
	if (IS_ERR(data->carveout))
		return dev_err_probe(dev, PTR_ERR(data->carveout),
				     "failed to map Zephyr carveout\n");

	data->carveout_phys = rmem->base;
	data->carveout_size = rmem->size;
	data->mpidr = mpidr;
	data->entry = entry;
	data->image_size = image_size;
	data->status_offset = status_offset;
	data->cpu_on_ret = -EAGAIN;
	data->mailmsg_state = R1_AMP_MAILMSG_UNARMED;
	data->session_result = -EAGAIN;
	data->stop_result = -EAGAIN;
	data->stop_notify_result = -EAGAIN;
	data->notify_inject_errno = -EIO;
	data->mailmsg_notify_endpoint.ops = &mailmsg_linux_notify_ops;
	data->mailmsg_notify_endpoint.context = data;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->affinity_work, r1_amp_affinity_work);
	INIT_WORK(&data->control_work, r1_amp_mailmsg_control_work);
	INIT_DELAYED_WORK(&data->lifecycle_timeout_work,
			  r1_amp_mailmsg_lifecycle_timeout_work);
	atomic_set(&data->mailmsg_user_open_count, 0);
	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++) {
		data->mailmsg_user[priority].parent = data;
		data->mailmsg_user[priority].priority = priority;
		init_waitqueue_head(&data->mailmsg_user[priority].rx_wait);
		atomic_set(&data->mailmsg_user[priority].rx_generation, 0);
		atomic_set(&data->mailmsg_user[priority].reader_open_count, 0);
	}
	ret = r1_amp_map_mbox_regs(dev, data);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, data);

	ret = r1_amp_request_mbox_channels(dev, data);
	if (ret)
		return ret;

	data->image_attr.attr.name = "image";
	data->image_attr.attr.mode = 0200;
	data->image_attr.size = image_size;
	data->image_attr.write = image_write;
	ret = sysfs_create_bin_file(&dev->kobj, &data->image_attr);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create image attribute\n");

	ret = device_create_file(dev, &dev_attr_status);
	if (ret)
		goto err_image;
	ret = device_create_file(dev, &dev_attr_mailmsg_stats);
	if (ret)
		goto err_status;
	ret = device_create_file(dev, &dev_attr_affinity_state);
	if (ret)
		goto err_mailmsg_stats;
	ret = device_create_file(dev, &dev_attr_start);
	if (ret)
		goto err_affinity_state;
	ret = device_create_file(dev, &dev_attr_rearm);
	if (ret)
		goto err_start;
	ret = device_create_file(dev, &dev_attr_ping);
	if (ret)
		goto err_rearm;
	ret = device_create_file(dev, &dev_attr_response);
	if (ret)
		goto err_ping;
	ret = device_create_file(dev, &dev_attr_doorbell);
	if (ret)
		goto err_response;
	ret = device_create_file(dev, &dev_attr_mailmsg_ping);
	if (ret)
		goto err_doorbell;
	ret = device_create_file(dev, &dev_attr_mailmsg_stop);
	if (ret)
		goto err_mailmsg_ping;
	ret = device_create_file(dev, &dev_attr_mailmsg_crc_inject);
	if (ret)
		goto err_mailmsg_stop;
	ret = device_create_file(dev, &dev_attr_mailmsg_notify_inject);
	if (ret)
		goto err_mailmsg_crc_inject;
	ret = device_create_file(dev, &dev_attr_mailmsg_queue_push);
	if (ret)
		goto err_mailmsg_notify_inject;
	ret = device_create_file(dev, &dev_attr_mailmsg_response);
	if (ret)
		goto err_mailmsg_queue_push;
	ret = r1_amp_register_mailmsg_user(dev, data);
	if (ret)
		goto err_mailmsg_response;

	dev_info(dev, "armed for MPIDR %#llx entry %#llx image %u bytes\n",
		 mpidr, entry, image_size);
	return 0;

err_mailmsg_response:
	device_remove_file(dev, &dev_attr_mailmsg_response);
err_mailmsg_queue_push:
	device_remove_file(dev, &dev_attr_mailmsg_queue_push);
err_mailmsg_notify_inject:
	device_remove_file(dev, &dev_attr_mailmsg_notify_inject);
err_mailmsg_crc_inject:
	device_remove_file(dev, &dev_attr_mailmsg_crc_inject);
err_mailmsg_stop:
	device_remove_file(dev, &dev_attr_mailmsg_stop);
err_mailmsg_ping:
	device_remove_file(dev, &dev_attr_mailmsg_ping);
err_doorbell:
	device_remove_file(dev, &dev_attr_doorbell);
err_response:
	device_remove_file(dev, &dev_attr_response);
err_ping:
	device_remove_file(dev, &dev_attr_ping);
err_rearm:
	device_remove_file(dev, &dev_attr_rearm);
err_start:
	device_remove_file(dev, &dev_attr_start);
err_affinity_state:
	device_remove_file(dev, &dev_attr_affinity_state);
err_mailmsg_stats:
	device_remove_file(dev, &dev_attr_mailmsg_stats);
err_status:
	device_remove_file(dev, &dev_attr_status);
err_image:
	sysfs_remove_bin_file(&dev->kobj, &data->image_attr);
	r1_amp_free_mbox_channels(data);
	return ret;
}

static const struct of_device_id r1_amp_cpu_on_of_match[] = {
	{ .compatible = "lzamp,amp-mailmsg" },
	{ .compatible = "youyeetoo,r1-amp-psci-cpu-on-heartbeat" },
	{ }
};
MODULE_DEVICE_TABLE(of, r1_amp_cpu_on_of_match);

static struct platform_driver r1_amp_cpu_on_driver = {
	.probe = r1_amp_cpu_on_probe,
	.driver = {
		.name = "lzamp-amp-mailmsg",
		.of_match_table = r1_amp_cpu_on_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(r1_amp_cpu_on_driver);

MODULE_DESCRIPTION("LZAMP PSCI CPU lifecycle and MailMsg transport");
MODULE_LICENSE("GPL");
