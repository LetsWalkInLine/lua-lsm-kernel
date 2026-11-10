// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "tests/test-common.h"
#include <linux/timekeeping.h>
#include "lsm.h"
#include "lsm_defs.h"

#define META "author='tests',description='budget bench',license='GPL-2.0',version=1,"
static void budget_benchmark(struct kunit *test)
{
	static const struct {
		const char *name, *setup, *body;
	} workloads[] = {
		{"allow", "", "return true"},
		{"arithmetic32", "", "local x=0;for i=1,32 do x=x+i end;return x>0"},
		{"arithmetic512", "", "local x=0;for i=1,512 do x=x+i end;return x>0"},
		{"table32", "", "local t={};for i=1,32 do t[i]=i end;return t[1]==1"},
		{"callback32", "local s=string.rep('a',32);", "return #string.gsub(s,'.',function(c)return c end)==32"},
		{"coroutine16", "", "local co=coroutine.create(function()for i=1,16 do coroutine.yield(i)end end);for i=1,16 do assert(coroutine.resume(co))end;return true"},
		{"lower64k", "local s=string.rep('A',65536);", "return #string.lower(s)==65536"},
		{"rules1024", "local t={};for i=1,1024 do t[i]='rule'..i end;", "return t[1024]=='rule1024'"},
	};
	char *code = kunit_kmalloc(test, 4096, GFP_KERNEL);
	int w, ctx, round, j;

	KUNIT_ASSERT_NOT_NULL(test, code);
	for (w=0; w<ARRAY_SIZE(workloads); w++) {
	    for (ctx=0; ctx<(w<6 ? 2 : 1); ctx++) {
		for (round=0; round<5; round++) {
			u64 start, cold, total;
			int ret = 0;
			snprintf(code, 4096, "%s return {name='budget_bench'," META
				"file_open=function()%s end}\n", workloads[w].setup, workloads[w].body);
			KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
			if (ctx)
				preempt_disable();
			start = ktime_get_ns();
			ret = lua_lsm_file_open(NULL);
			cold = ktime_get_ns() - start;
			start = ktime_get_ns();
			for (j=0; j<256; j++)
				ret |= lua_lsm_file_open(NULL);
			total = ktime_get_ns() - start;
			if (ctx)
				preempt_enable();
			KUNIT_ASSERT_EQ(test, ret, 0);
			kunit_info(test, "BUDGET_BENCH name=%s ctx=%s round=%d cold_ns=%llu warm_ns=%llu\n",
				workloads[w].name, ctx ? "atomic" : "task", round, cold, total/256);
			KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("budget_bench"), 0);
		}
	    }
	}
}

static struct kunit_case cases[] = { KUNIT_CASE(budget_benchmark), {} };
static struct kunit_suite suite = {.name="lua-lsm-budget-benchmark", .init=lua_lsm_test_init, .test_cases=cases};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
