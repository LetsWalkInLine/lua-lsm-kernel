// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "open-check.h"

int main(int argc, char **argv)
{
	static const char * const errors[] = {
		"loop",	       "pcall", "xpcall",    "handler",
		"environment", "hook",	"coroutine", "callback"
	};
	int round;
	size_t i;

	if (argc > 1 && !strcmp(argv[1], "lazy")) {
		pid_t child = fork();
		int status;

		if (child < 0)
			return 1;
		if (!child)
			_exit(check("/budget-ok", EIO) ||
			      check("/budget-ok", EIO));
		if (waitpid(child, &status, 0) != child)
			return 1;
		return !WIFEXITED(status) || WEXITSTATUS(status);
	}
	if (argc > 1 && !strcmp(argv[1], "unload"))
		return check("/budget-loop", 0) || check("/budget-callback", 0);
	for (round = 0; round < 25; round++) {
		for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
			char path[64];

			snprintf(path, sizeof(path), "/budget-%s", errors[i]);
			if (check(path, EIO) || check("/budget-ok", 0))
				return 1;
		}
		if (check("/budget-return", 0) || check("/budget-caught", 0) ||
		    check("/budget-deny", EACCES))
			return 1;
	}
	return 0;
}
