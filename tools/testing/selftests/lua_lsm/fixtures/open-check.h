// SPDX-License-Identifier: GPL-2.0-only
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
