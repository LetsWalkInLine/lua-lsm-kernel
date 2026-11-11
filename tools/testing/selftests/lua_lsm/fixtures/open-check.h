/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_LSM_OPEN_CHECK_H
#define LUA_LSM_OPEN_CHECK_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int check(const char *path, int expected)
{
	int fd = open(path, O_RDONLY);
	int got = fd < 0 ? errno : 0;

	if (fd >= 0)
		close(fd);
	if (got == expected)
		return 0;
	fprintf(stderr, "%s: expected errno %d, got %d\n", path, expected, got);
	return 1;
}

#endif
