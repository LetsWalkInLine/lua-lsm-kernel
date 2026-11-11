// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Start receiver before signalling readiness. Sender uses the peer netns. */
int main(int argc, char **argv)
{
	struct sockaddr_in addr = { .sin_family = AF_INET,
				    .sin_port = htons(19391) };
	struct pollfd p = { .events = POLLIN };
	int fd, n;
	char byte = 0;

	if (argc != 3)
		return 1;
	if (!strcmp(argv[1], "send")) {
		n = open("/run/netns/peer", O_RDONLY);
		if (n < 0 || setns(n, CLONE_NEWNET))
			return 1;
		close(n);
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr);
		return fd < 0 ||
		       sendto(fd, "x", 1, 0, (void *)&addr, sizeof(addr)) != 1;
	}
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr);
	if (fd < 0 || bind(fd, (void *)&addr, sizeof(addr))) {
		perror("bind");
		return 1;
	}
	n = open("/tmp/rx-ready", O_WRONLY | O_CREAT, 0600);
	if (n < 0)
		return 1;
	close(n);
	p.fd = fd;
	n = poll(&p, 1, 1500);
	if (n < 0)
		return 1;
	if (!strcmp(argv[2], "drop"))
		return n != 0;
	return n != 1 || recv(fd, &byte, 1, 0) != 1 || byte != 'x';
}
