/* SPDX-License-Identifier: MIT */
#include "mailmsg_mailbox0.h"

#define MAILMSG_MAILBOX0_DOORBELL 0xa2b10000U

int mailmsg_mailbox0_notify(void *context, enum mailmsg_priority priority)
{
	struct mailmsg_mailbox0 *adapter = context;

	if (!adapter || !adapter->send || priority >= MAILMSG_PRIORITY_COUNT)
		return -1;

	return adapter->send(adapter->context, priority,
			     MAILMSG_MAILBOX0_DOORBELL | priority, priority);
}

const struct mailmsg_notify_ops mailmsg_mailbox0_notify_ops = {
	.notify = mailmsg_mailbox0_notify,
};
