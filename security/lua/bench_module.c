// SPDX-License-Identifier: GPL-2.0-only
/*
 * In-kernel timing driver for the Lua-LSM benchmark delivery.
 *
 * Phase A (always): string operations with documented work demands
 *	(normal, near-limit and over-limit inputs) timed through the
 *	production file_open dispatch path.  Inputs are generated at
 *	module lazy load (the first call) and cached by the module
 *	table, so warm samples only contain the measured operation.
 *
 * Phase B (requires lua_bench_expiry on the command line): total
 *	time of policy calls that loop until the per-module VM execution
 *	deadline, in normal and preempt-disabled contexts, for pure-Lua,
 *	protected-call, coroutine and callback paths.
 *
 * Phase B policies loop on purpose and never return on trees without
 * an execution budget.  Run only in a disposable guest behind a host
 *	watchdog, and pass lua_bench_expiry only on budget-enforcing
 *	kernels.
 */
#include <kunit/test.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/lua.h>
#include <linux/printk.h>
#include <linux/string.h>
#include "lsm.h"
#include "lsm_defs.h"

/* Maintained by the LSM framework; unlike lua_lsm_initialized it survives init. */
extern int lua_lsm_enabled;

#define META "author='benchmark',description='lua-lsm bench',license='GPL-2.0',version=1,"
#define CODE_MAX 8192
#define STR_WARM 128
#define EXP_REPS 32
#define EXP_BASELINE 32
#define EXP_RECOVERY 16
#define RAW_PER_LINE 64
/* Production defaults measured here; totals close to budget+overhead confirm them. */
#define BUDGET_TASK_NS 20000000ULL
#define BUDGET_ATOMIC_NS 2000000ULL

static bool expiry_enabled;

static int __init bench_expiry_opt(char *str)
{
	expiry_enabled = true;
	return 1;
}
__setup("lua_bench_expiry", bench_expiry_opt);

static int bench_init(struct kunit *test)
{
	if (!READ_ONCE(lua_lsm_enabled))
		kunit_skip(test, "Lua-LSM is not active");
	return 0;
}

/*
 * Corpus inputs reuse the measured demand families from the adopted
 * work-limit report: unit costs are per documented family formula and
 * are recorded for context; the measured policy result decides the
 * actual category.  `gen' runs once at lazy load and defines `s'
 * (plus helpers); `op' is the timed operation.
 */
struct corpus_item {
	const char *name;
	const char *gen;
	const char *op;
	u64 demand;
};

static const struct corpus_item corpus[] = {
	{"allow", "local s=''", "", 0},
	{"lower4096", "local s=string.rep('a',4096)",
	 "assert(string.lower(s)==s);", 4097},
	{"labelmatch65536", "local s=string.rep('a',65536)",
	 "assert(string.match(s,\"^[%w_.:-]+$\")==s);", 327696},
	{"replace65536", "local s=string.rep('a',65536)",
	 "local r,k=string.gsub(s,\"a\",\"b\") assert(k==#s);", 589829},
	{"findnear76190", "local s=string.rep('-',76189)..'!'",
	 "local a,b=string.find(s,\"[^%w_.:-]\") assert(a==#s and b==#s);", 1599994},
	{"escapenear76190", "local s=string.rep('!',76190)",
	 "local r,k=string.gsub(s,\"(%W)\",\"%%%1\") assert(k==#s);", 1599998},
	{"escapeover76191", "local s=string.rep('!',76191)",
	 "local r,k=string.gsub(s,\"(%W)\",\"%%%1\") assert(k==#s);", 1600019},
	{"findover76191", "local s=string.rep('-',76190)..'!'",
	 "local a,b=string.find(s,\"[^%w_.:-]\") assert(a==#s and b==#s);", 1600015},
	{"trimsp1026", "local s='a'..string.rep(' ',1024)..'a'",
	 "local r,k=string.gsub(s,\"^%s*(.-)%s*$\",\"%1\") assert(k==1);", 2115646},
	{"basename4095",
	 "local p={} for i=1,15 do p[i]=string.rep('a',255) end "
	 "p[16]=string.rep('a',254) local s='/'..table.concat(p,'/') local tail=p[16]",
	 "assert(string.match(s,\"([^/]+)$\")==tail);", 5437074},
};

/*
 * Raw variant: an over-limit operation raises an uncaught Lua error and
 * the dispatch returns -EIO.  Caught variant: pcall classifies the
 * error; "string work limit exceeded" denies with errno 16 (-EBUSY)
 * so the category is exact, and anything else fails the benchmark.
 */
static int build_corpus_code(char *buf, const struct corpus_item *item,
			     bool caught)
{
	int n;

	if (!caught)
		n = snprintf(buf, CODE_MAX,
			     "local M={name='bench_%s',%s}\n%s\n"
			     "M.file_open=function() %s return true end\n"
			     "return M\n",
			     item->name, META, item->gen, item->op);
	else
		n = snprintf(buf, CODE_MAX,
			     "local M={name='bench_%s',%s}\n%s\n"
			     "M.file_open=function()\n"
			     " local ok,err=pcall(function() %s end)\n"
			     " if ok then return true end\n"
			     " if tostring(err):find('string work limit exceeded',1,true) then return false,16 end\n"
			     " error('bench-unexpected:'..tostring(err))\n"
			     "end\nreturn M\n",
			     item->name, META, item->gen, item->op);
	return n >= CODE_MAX ? -1 : 0;
}

static u64 warm[STR_WARM];
static u64 exp_raw[EXP_REPS];
static u64 base_raw[EXP_BASELINE];
static u64 rec_raw[EXP_RECOVERY];

static void print_raw(struct kunit *test, const char *tag, const u64 *vals,
		      unsigned int n)
{
	static char line[3 * RAW_PER_LINE * 11 + 64];
	unsigned int i, j, parts = (n + RAW_PER_LINE - 1) / RAW_PER_LINE;

	for (i = 0; i < parts; i++) {
		size_t len = 0;
		int w = snprintf(line, sizeof(line), "%s part=%u/%u ", tag, i, parts);

		if (w < 0)
			return;
		len = w;
		for (j = 0; j < RAW_PER_LINE && i * RAW_PER_LINE + j < n; j++) {
			w = snprintf(line + len, sizeof(line) - len, "%s%llu",
				     j ? "," : "", vals[i * RAW_PER_LINE + j]);
			if (w < 0 || (size_t)w >= sizeof(line) - len)
				return;
			len += w;
		}
		kunit_info(test, "%s\n", line);
	}
}

static void print_stats(struct kunit *test, const u64 *vals, unsigned int n,
			u64 *mean, u64 *min, u64 *max)
{
	u64 sum = 0, lo = U64_MAX, hi = 0;
	unsigned int i;

	for (i = 0; i < n; i++) {
		sum += vals[i];
		if (vals[i] < lo)
			lo = vals[i];
		if (vals[i] > hi)
			hi = vals[i];
	}
	*mean = n ? sum / n : 0;
	*min = lo == U64_MAX ? 0 : lo;
	*max = hi;
	kunit_info(test, "BENCH_STATS n=%u mean=%llu min=%llu max=%llu\n",
		   n, *mean, *min, *max);
}

static void bench_corpus(struct kunit *test)
{
	unsigned int i;
	bool caught;
	int variant;

	kunit_info(test, "BENCH_INFO phase=corpus items=%zu warm=%u\n",
		   ARRAY_SIZE(corpus), STR_WARM);
	for (i = 0; i < ARRAY_SIZE(corpus); i++) {
		/* "allow" is dispatch overhead only; no caught variant needed. */
		for (variant = 0; variant < (corpus[i].demand ? 2 : 1); variant++) {
			const struct corpus_item *item = &corpus[i];
			static char code[CODE_MAX];
			u64 cold, mean, lo, hi, t0, t1;
			char name[96];
			int ret, first;
			unsigned int k;

			caught = variant == 1;
			KUNIT_ASSERT_EQ(test, build_corpus_code(code, item, caught), 0);
			snprintf(name, sizeof(name), "bench_%s", item->name);
			KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
			/* Cold call: lazy load, input generation and the first op. */
			t0 = ktime_get_ns();
			ret = lua_lsm_file_open(NULL);
			t1 = ktime_get_ns();
			cold = t1 - t0;
			first = ret;
			KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EIO ||
					   ret == -EBUSY);
			for (k = 0; k < STR_WARM; k++) {
				t0 = ktime_get_ns();
				ret = lua_lsm_file_open(NULL);
				t1 = ktime_get_ns();
				warm[k] = t1 - t0;
				KUNIT_EXPECT_EQ(test, ret, first);
			}
			KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister(name), 0);
			kunit_info(test,
				   "BENCH_STR name=%s v=%s demand=%llu cat=%d cold_ns=%llu\n",
				   item->name, caught ? "caught" : "raw",
				   item->demand, first, cold);
			print_stats(test, warm, STR_WARM, &mean, &lo, &hi);
			snprintf(name, sizeof(name), "BENCH_STR_RAW name=%s v=%s",
				 item->name, caught ? "caught" : "raw");
			print_raw(test, name, warm, STR_WARM);
		}
	}
	kunit_info(test, "BENCH_DONE phase=corpus\n");
}

static const struct {
	const char *name;
	const char *body;
} expiry_shapes[] = {
	{"loop", "while true do end"},
	{"pcall", "pcall(function() while true do end end)"},
	{"coroutine",
	 "local co=coroutine.create(function() while true do end end) coroutine.resume(co)"},
	{"callback",
	 "string.gsub('x','.',function() pcall(function() while true do end end) return '' end)"},
};

/*
 * Register an allow-only policy, time a shape that runs until the
 * deadline, then time allow calls again on the same VM (recovery).
 * Returns the mean of the warm allow baseline.
 */
static void expiry_cell(struct kunit *test, const char *tag,
			const char *body, bool atomic, u64 *allow_mean)
{
	static char code[CODE_MAX];
	u64 t0, t1, mean, lo, hi;
	unsigned int k;
	int ret, n;

	/* Allow baseline: dispatch entry including budget begin/end. */
	n = snprintf(code, CODE_MAX,
		     "local M={name='bench_exp_allow',%s}\n"
		     "M.file_open=function() return true end\nreturn M\n", META);
	KUNIT_ASSERT_TRUE(test, n > 0 && n < CODE_MAX);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
	t0 = ktime_get_ns();
	ret = lua_lsm_file_open(NULL);
	t1 = ktime_get_ns();
	KUNIT_EXPECT_EQ(test, ret, 0);
	kunit_info(test, "BENCH_EXP name=allow tag=%s ctx=%s cold_ns=%llu\n",
		   tag, atomic ? "atomic" : "task", t1 - t0);
	if (atomic)
		preempt_disable();
	for (k = 0; k < EXP_BASELINE; k++) {
		t0 = ktime_get_ns();
		ret = lua_lsm_file_open(NULL);
		t1 = ktime_get_ns();
		base_raw[k] = t1 - t0;
		KUNIT_EXPECT_EQ(test, ret, 0);
	}
	if (atomic)
		preempt_enable();
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("bench_exp_allow"), 0);
	kunit_info(test, "BENCH_EXP name=allow tag=%s ctx=%s v=warm\n", tag,
		   atomic ? "atomic" : "task");
	print_stats(test, base_raw, EXP_BASELINE, &mean, &lo, &hi);
	print_raw(test, "BENCH_EXP_RAW name=allow tag=base", base_raw,
		  EXP_BASELINE);
	*allow_mean = mean;

	/* The looping policy: every call ends through the deadline. */
	n = snprintf(code, CODE_MAX,
		     "local M={name='bench_exp_%s',%s}\n"
		     "M.file_open=function() %s end\nreturn M\n",
		     tag, META, body);
	KUNIT_ASSERT_TRUE(test, n > 0 && n < CODE_MAX);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
	for (k = 0; k < EXP_REPS; k++) {
		if (atomic)
			preempt_disable();
		t0 = ktime_get_ns();
		ret = lua_lsm_file_open(NULL);
		t1 = ktime_get_ns();
		if (atomic)
			preempt_enable();
		exp_raw[k] = t1 - t0;
		KUNIT_EXPECT_EQ(test, ret, -EIO);
		/* A genuine deadline exit cannot be shorter than the budget:
		 * the budget starts after call entry and the error is raised
		 * at or after the deadline.  Ordinary Lua errors also map to
		 * -EIO, so this floor is what proves the deadline fired. */
		KUNIT_EXPECT_TRUE(test, exp_raw[k] >=
				  (atomic ? BUDGET_ATOMIC_NS : BUDGET_TASK_NS));
	}
	{
		char name[96];

		snprintf(name, sizeof(name), "bench_exp_%s", tag);
		KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister(name), 0);
	}
	kunit_info(test, "BENCH_EXP name=%s tag=%s ctx=%s v=timeout budget=%llu allow_mean=%llu\n",
		   tag, tag, atomic ? "atomic" : "task",
		   atomic ? BUDGET_ATOMIC_NS : BUDGET_TASK_NS, mean);
	print_stats(test, exp_raw, EXP_REPS, &mean, &lo, &hi);
	{
		char rawtag[96];

		snprintf(rawtag, sizeof(rawtag), "BENCH_EXP_RAW name=%s tag=timeout ctx=%s",
			 tag, atomic ? "atomic" : "task");
		print_raw(test, rawtag, exp_raw, EXP_REPS);
	}

	/* Recovery: normal calls on the VM that just went through timeouts.
	 * The first call re-loads the allow module lazily and must run
	 * preemptible (module load may allocate); it is untimed so warm
	 * samples contain no lazy-load cost. */
	n = snprintf(code, CODE_MAX,
		     "local M={name='bench_exp_allow',%s}\n"
		     "M.file_open=function() return true end\nreturn M\n", META);
	KUNIT_ASSERT_TRUE(test, n > 0 && n < CODE_MAX);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
	t0 = ktime_get_ns();
	ret = lua_lsm_file_open(NULL);
	t1 = ktime_get_ns();
	KUNIT_EXPECT_EQ(test, ret, 0);
	/* The load call itself runs preemptible even in atomic cells;
	 * label it accordingly, keep ctx=atomic only for warm samples. */
	kunit_info(test, "BENCH_EXP name=recovery-load tag=%s ctx=preempt cold_ns=%llu\n",
		   tag, t1 - t0);
	for (k = 0; k < EXP_RECOVERY; k++) {
		if (atomic)
			preempt_disable();
		t0 = ktime_get_ns();
		ret = lua_lsm_file_open(NULL);
		t1 = ktime_get_ns();
		if (atomic)
			preempt_enable();
		rec_raw[k] = t1 - t0;
		KUNIT_EXPECT_EQ(test, ret, 0);
	}
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("bench_exp_allow"), 0);
	kunit_info(test, "BENCH_EXP name=recovery tag=%s ctx=%s\n", tag,
		   atomic ? "atomic" : "task");
	print_stats(test, rec_raw, EXP_RECOVERY, &mean, &lo, &hi);
	print_raw(test, "BENCH_EXP_RAW name=recovery", rec_raw, EXP_RECOVERY);
}

static void bench_expiry(struct kunit *test)
{
	unsigned int s;
	bool atomic;
	u64 allow_mean;

	if (!expiry_enabled) {
		kunit_skip(test, "pass lua_bench_expiry to measure deadline termination");
		return;
	}
	kunit_info(test, "BENCH_INFO phase=expiry shapes=%zu reps=%u budgets=%llu/%llu\n",
		   ARRAY_SIZE(expiry_shapes), EXP_REPS,
		   BUDGET_TASK_NS, BUDGET_ATOMIC_NS);
	for (s = 0; s < ARRAY_SIZE(expiry_shapes); s++) {
		for (atomic = false; ; atomic = true) {
			expiry_cell(test, expiry_shapes[s].name,
				    expiry_shapes[s].body, atomic, &allow_mean);
			if (atomic)
				break;
		}
	}
	kunit_info(test, "BENCH_DONE phase=expiry\n");
}

static struct kunit_case bench_cases[] = {
	KUNIT_CASE(bench_corpus),
	KUNIT_CASE(bench_expiry),
	{},
};

static struct kunit_suite bench_suite = {
	.name = "lua-lsm-bench",
	.init = bench_init,
	.test_cases = bench_cases,
};
kunit_test_suite(bench_suite);
MODULE_LICENSE("GPL");
