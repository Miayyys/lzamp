/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "mailmsg_notify.h"

static int last_priority = -1;
static int notify_result;

static int test_notify(void *context, enum mailmsg_priority priority)
{
	(void)context;
	last_priority = priority;
	return notify_result;
}

int main(void)
{
	const struct mailmsg_notify_ops ops = { .notify = test_notify };
	const struct mailmsg_notify_endpoint endpoint = { .ops = &ops };

	notify_result = MAILMSG_NOTIFY_SENT;
	assert(mailmsg_notify(&endpoint, MAILMSG_PRIO_CONTROL) ==
	       MAILMSG_NOTIFY_SENT);
	assert(last_priority == MAILMSG_PRIO_CONTROL);
	assert(mailmsg_notify_accepted(MAILMSG_NOTIFY_SENT));
	assert(mailmsg_notify_state_from_result(MAILMSG_NOTIFY_SENT) ==
	       MAILMSG_NOTIFY_STATE_SENT);

	notify_result = MAILMSG_NOTIFY_COALESCED;
	assert(mailmsg_notify(&endpoint, MAILMSG_PRIO_CONTROL) ==
	       MAILMSG_NOTIFY_COALESCED);
	assert(mailmsg_notify_accepted(MAILMSG_NOTIFY_COALESCED));
	assert(mailmsg_notify_state_from_result(MAILMSG_NOTIFY_COALESCED) ==
	       MAILMSG_NOTIFY_STATE_COALESCED);
	assert(!mailmsg_notify_accepted(-1));
	assert(mailmsg_notify_state_from_result(-1) ==
	       MAILMSG_NOTIFY_STATE_FAILED);
	assert(mailmsg_notify(&endpoint, MAILMSG_PRIORITY_COUNT) < 0);
	return 0;
}
