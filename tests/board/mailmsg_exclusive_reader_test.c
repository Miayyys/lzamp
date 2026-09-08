/* SPDX-License-Identifier: MIT */
/* Verify that one priority has one Linux-side MailMsg receiver at a time. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mailmsg_user.h"

int main(void)
{
	struct mailmsg_user_frame frame;
	struct pollfd poll_fd;
	int first_fd, second_fd;
	ssize_t ret;

	first_fd = open("/dev/mailmsg-p0", O_RDWR | O_NONBLOCK);
	if (first_fd < 0) {
		perror("open first reader");
		return 1;
	}
	second_fd = open("/dev/mailmsg-p0", O_RDWR | O_NONBLOCK);
	if (second_fd < 0) {
		perror("open second reader");
		close(first_fd);
		return 1;
	}

	poll_fd.fd = first_fd;
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	if (poll(&poll_fd, 1, 0) < 0 || poll_fd.revents & POLLERR) {
		perror("claim first reader");
		goto fail;
	}

	ret = read(second_fd, &frame, sizeof(frame));
	if (ret >= 0 || errno != EBUSY) {
		fprintf(stderr, "second reader: expected EBUSY, got ret=%zd errno=%d\n",
			ret, errno);
		goto fail;
	}

	close(first_fd);
	first_fd = -1;
	poll_fd.fd = second_fd;
	poll_fd.revents = 0;
	if (poll(&poll_fd, 1, 0) < 0 || poll_fd.revents & POLLERR) {
		perror("claim replacement reader");
		goto fail;
	}

	ret = read(second_fd, &frame, sizeof(frame));
	if (ret >= 0 || errno != EAGAIN) {
		fprintf(stderr, "replacement reader: expected EAGAIN, got ret=%zd errno=%d\n",
			ret, errno);
		goto fail;
	}

	puts("exclusive-reader=pass second=EBUSY handoff=EAGAIN");
	close(second_fd);
	return 0;

fail:
	if (first_fd >= 0)
		close(first_fd);
	close(second_fd);
	return 1;
}
