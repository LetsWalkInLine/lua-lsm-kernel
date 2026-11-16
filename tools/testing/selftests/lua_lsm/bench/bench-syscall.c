// SPDX-License-Identifier: GPL-2.0-only
/*
 * Long-lived openat benchmark for the Lua-LSM benchmark delivery.
 *
 * One scenario per boot, selected by the guest init from the kernel
 * command line.  Cold samples come from fresh forked children (each
 * child's first hook call pays module lazy load in its own task VM),
 * then the long-lived parent warms up, records per-call openat
 * latencies and finally throughput of openat+close blocks.  The
 * "eio" expectation times a timeout policy instead: every call
 * returns -EIO after the VM deadline.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RAW_PER_LINE 32

static int64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		perror("clock_gettime");
		exit(2);
	}
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int openat_raw(const char *path)
{
	return syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
}

static int cmp64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

static double sqrt_(double x);

static void print_raw(const char *tag, const int64_t *v, size_t n)
{
	size_t i, j, parts = (n + RAW_PER_LINE - 1) / RAW_PER_LINE;

	for (i = 0; i < parts; i++) {
		printf("%s part=%zu/%zu", tag, i, parts);
		for (j = 0; j < RAW_PER_LINE && i * RAW_PER_LINE + j < n; j++)
			printf("%s%" PRId64, j ? "," : "",
			       v[i * RAW_PER_LINE + j]);
		printf("\n");
	}
}

/* mean/stdev/min/p50/p90/p95/p99/max over a sorted copy of the samples. */
static void print_stats(const char *tag, const int64_t *v, size_t n)
{
	int64_t *s = malloc(n * sizeof(*s));
	double sum = 0, sum2 = 0;
	size_t i;

	if (!s) {
		printf("%s ERROR malloc\n", tag);
		exit(2);
	}
	memcpy(s, v, n * sizeof(*s));
	qsort(s, n, sizeof(*s), cmp64);
	for (i = 0; i < n; i++) {
		sum += s[i];
		sum2 += (double)s[i] * s[i];
	}
	printf("%s n=%zu mean=%.1f stdev=%.1f min=%" PRId64 " p50=%" PRId64
	       " p90=%" PRId64 " p95=%" PRId64 " p99=%" PRId64 " max=%" PRId64 "\n",
	       tag, n, sum / n,
	       n > 1 ? sqrt_(sum2 / n - (sum / n) * (sum / n)) : 0.0,
	       s[0], s[n / 2], s[n * 90 / 100], s[n * 95 / 100],
	       s[n * 99 / 100], s[n - 1]);
	free(s);
}

static double sqrt_(double x)
{
	double r = x / 2 + 0.5;
	int i;

	if (x <= 0)
		return 0;
	for (i = 0; i < 40; i++)
		r = (r + x / r) / 2;
	return r;
}

static int check_ret(int ret, int expect_eio)
{
	if (expect_eio)
		return ret < 0 && errno == EIO;
	return ret >= 0;
}

int main(int argc, char **argv)
{
	const char *tag, *path;
	int64_t *samples, *blocks;
	long ncold, nwarm, nsample, nblocks, bsize;
	int expect_eio, bad = 0;
	long i, j;

	if (argc != 9) {
		fprintf(stderr,
			"usage: %s TAG PATH ok|eio NCOLD NWARMUP NSAMPLE NBLOCKS BLOCKSIZE\n",
			argv[0]);
		return 2;
	}
	setbuf(stdout, NULL);
	tag = argv[1];
	path = argv[2];
	expect_eio = !strcmp(argv[3], "eio");
	ncold = atol(argv[4]);
	nwarm = atol(argv[5]);
	nsample = atol(argv[6]);
	nblocks = atol(argv[7]);
	bsize = atol(argv[8]);
	printf("BENCH_SYS tag=%s path=%s expect=%s cold=%ld warmup=%ld sample=%ld blocks=%ld blocksize=%ld\n",
	       tag, path, expect_eio ? "eio" : "ok", ncold, nwarm, nsample,
	       nblocks, bsize);

	/* Phase 1: cold calls in fresh children (first hook call per task). */
	for (i = 0; i < ncold; i++) {
		pid_t child = fork();
		int64_t t0, t1;
		int status, ret;

		if (child < 0) {
			perror("fork");
			return 2;
		}
		if (!child) {
			t0 = now_ns();
			ret = openat_raw(path);
			t1 = now_ns();
			printf("BENCH_COLD tag=%s i=%ld ns=%" PRId64 " ret=%d errno=%d\n",
			       tag, i, t1 - t0, ret, ret < 0 ? errno : 0);
			_exit(ret >= 0 || (expect_eio && errno == EIO) ? 0 : 1);
		}
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			printf("BENCH_COLD tag=%s i=%ld FAIL status=%d\n", tag, i, status);
			return 1;
		}
	}

	/* Phase 2: warmup in the long-lived parent. */
	for (i = 0; i < nwarm; i++) {
		int ret = openat_raw(path);

		if (!check_ret(ret, expect_eio)) {
			printf("BENCH_WARMUP tag=%s i=%ld FAIL ret=%d errno=%d\n",
			       tag, i, ret, errno);
			return 1;
		}
		if (ret >= 0)
			close(ret);
	}

	/* Phase 3: per-call openat latency samples (close outside timing). */
	samples = malloc(nsample * sizeof(*samples));
	if (!samples)
		return 2;
	for (i = 0; i < nsample; i++) {
		int64_t t0, t1;
		int ret;

		t0 = now_ns();
		ret = openat_raw(path);
		t1 = now_ns();
		if (!check_ret(ret, expect_eio)) {
			printf("BENCH_SAMPLE tag=%s i=%ld FAIL ret=%d errno=%d\n",
			       tag, i, ret, errno);
			return 1;
		}
		if (ret >= 0)
			close(ret);
		samples[i] = t1 - t0;
	}
	print_stats("BENCH_STATS tag=samples", samples, nsample);
	print_raw("BENCH_RAW tag=samples", samples, nsample);

	/* Phase 4: throughput of openat+close blocks. */
	blocks = malloc(nblocks * sizeof(*blocks));
	if (!blocks)
		return 2;
	for (i = 0; i < nblocks; i++) {
		int64_t t0 = now_ns();

		for (j = 0; j < bsize; j++) {
			int ret = openat_raw(path);

			if (!check_ret(ret, expect_eio)) {
				printf("BENCH_THROUGH tag=%s i=%ld FAIL ret=%d errno=%d\n",
				       tag, i, ret, errno);
				return 1;
			}
			if (ret >= 0)
				close(ret);
		}
		blocks[i] = now_ns() - t0;
	}
	print_stats("BENCH_STATS tag=blocks", blocks, nblocks);
	print_raw("BENCH_RAW tag=blocks", blocks, nblocks);
	printf("BENCH_SYS tag=%s DONE bad=%d\n", tag, bad);
	return 0;
}
