/* SPDX-License-Identifier: MIT */
/* Verify that a pre-existing reader observes a MailMsg endpoint going offline. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mailmsg_user.h"

int main(void)
{
	struct mailmsg_user_frame frame;
	struct pollfd pollfd = {
		.fd = -1,
		.events = POLLIN,
	};
	int read_errno;
	ssize_t ret;

	pollfd.fd = open("/dev/mailmsg-p1", O_RDWR | O_CLOEXEC);
	if (pollfd.fd < 0) {
		perror("open /dev/mailmsg-p1");
		return 1;
	}

	puts("waiting-for-offline");
	ret = poll(&pollfd, 1, -1);
	if (ret != 1) {
		perror("poll");
		close(pollfd.fd);
		return 1;
	}
	printf("poll-revents=%#x\n", pollfd.revents);

	memset(&frame, 0, sizeof(frame));
	ret = read(pollfd.fd, &frame, sizeof(frame));
	if (ret >= 0) {
		fprintf(stderr, "read unexpectedly returned %zd\n", ret);
		close(pollfd.fd);
		return 1;
	}
	read_errno = errno;
	printf("read-errno=%d (%s)\n", read_errno, strerror(read_errno));
	close(pollfd.fd);

	return (pollfd.revents & (POLLHUP | POLLERR)) && read_errno == ENOLINK ? 0 : 1;
}
