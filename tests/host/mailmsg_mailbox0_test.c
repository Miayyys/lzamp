/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "mailmsg_mailbox0.h"

static uint32_t channel, command, data;
static int send_result;

static int fake_send(void *context, uint32_t ch, uint32_t cmd, uint32_t value)
{
	(void)context;
	channel = ch;
	command = cmd;
	data = value;
	return send_result;
}

int main(void)
{
	struct mailmsg_mailbox0 adapter = { .send = fake_send };
	struct mailmsg_notify_endpoint endpoint = {
		.ops = &mailmsg_mailbox0_notify_ops, .context = &adapter,
	};

	send_result = MAILMSG_NOTIFY_SENT;
	assert(mailmsg_notify(&endpoint, MAILMSG_PRIO_NORMAL) ==
	       MAILMSG_NOTIFY_SENT);
	assert(channel == 2);
	assert(command == 0xa2b10002U);
	assert(data == 2);

	send_result = MAILMSG_NOTIFY_COALESCED;
	assert(mailmsg_notify(&endpoint, MAILMSG_PRIO_BEST_EFFORT) ==
	       MAILMSG_NOTIFY_COALESCED);
	assert(channel == 3);
	assert(command == 0xa2b10003U);
	assert(data == 3);
	return 0;
}
