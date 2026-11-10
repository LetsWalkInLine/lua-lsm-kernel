// SPDX-License-Identifier: GPL-2.0-only
/* Return-hook lifetime regression, using the production dispatcher. */
#include <kunit/test.h>
#include "tests/test-common.h"
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include "lsm.h"
#include "lsm_defs.h"
#include "../../lib/lua/lstate.h"

#define META "author='tests',description='review',license='GPL-2.0',version=1,"

static void register_hook(struct kunit *test, const char *body)
{
	char *code = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, code);
	snprintf(code, 4096, "local M={name='return_review'," META "hit=0};"
		"local function f()M.hit=M.hit+1;%s end;M.file_open=f;return M\n", body);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
}

static void expect_flag(struct kunit *test, const char *name, bool value)
{
	lua_State *L = lvm_get();
	KUNIT_ASSERT_NOT_NULL(test, L);
	KUNIT_EXPECT_EQ(test, G(L)->budget_active, 0);
	KUNIT_EXPECT_EQ(test, G(L)->budget_expired, 0);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 0);
	KUNIT_EXPECT_TRUE(test, lua_gethook(L) == NULL);
	lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
	lua_getfield(L, -1, "return_review");
	lua_getfield(L, -1, name);
	KUNIT_EXPECT_EQ(test, (bool)lua_toboolean(L, -1), value);
	lua_settop(L, 0);
	lvm_put(L);
}

static void return_hooks_end_with_module(struct kunit *test)
{
	const char *tails[] = {"M.escaped=true", "M.escaped=true;while true do end"};
	int n, i;

#ifdef VM_BUDGET_REVIEW_NEGATIVE_CONTROL
	n = 1; /* The broken baseline may run the finite callback; never hang it. */
#else
	n = ARRAY_SIZE(tails);
#endif
	for (i=0; i<n; i++) {
		char *body = kunit_kmalloc(test, 2048, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, body);
		snprintf(body, 2048, "local armed=false;debug.sethook(function(e)"
			"if armed then %s elseif debug.getinfo(2,'f').func==f then "
			"armed=true;M.armed=true end end,'r');return true", tails[i]);
		register_hook(test, body);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
		expect_flag(test, "armed", true);
		expect_flag(test, "escaped", false);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
		expect_flag(test, "escaped", false);
		KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("return_review"), 0);
	}
}

static void return_hook_loop_still_times_out(struct kunit *test)
{
	register_hook(test, "if M.hit>1 then return true end;"
		"debug.sethook(function()while true do end end,'r');return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	expect_flag(test, "escaped", false);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("return_review"), 0);
}

static void return_hook_isolation_between_modules(struct kunit *test)
{
	static const char second[] = "return {name='return_second'," META
		"file_open=function()assert(debug.gethook()==nil);return true end}\n";
	register_hook(test, "local armed=false;debug.sethook(function(e)"
		"if armed then M.escaped=true elseif debug.getinfo(2,'f').func==f then "
		"armed=true;M.armed=true end end,'r');return nil");
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(second, sizeof(second)-1), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	expect_flag(test, "armed", true);
	expect_flag(test, "escaped", false);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("return_second"), 0);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("return_review"), 0);
}

static struct kunit_case cases[] = {
	KUNIT_CASE(return_hooks_end_with_module),
	KUNIT_CASE(return_hook_loop_still_times_out),
	KUNIT_CASE(return_hook_isolation_between_modules),
	{}
};
static struct kunit_suite suite = {.name="lua-lsm-budget-return-review", .init=lua_lsm_test_init, .test_cases=cases};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
