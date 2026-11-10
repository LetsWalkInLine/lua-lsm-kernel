// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "open-check.h"

int main(int argc, char **argv)
{
	int i;

	if (argc > 1)
		return check(argv[1], argc > 2 ? EACCES : 0);
	for (i = 0; i < 10; i++)
		if (check("/strings-ok", 0) || check("/strings-caught", 0) ||
		    check("/strings-uncaught", EIO) || check("/strings-ok", 0))
			return 1;
	return 0;
}
