// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "tests/test-common.h"
#include "lsm.h"
#include "lsm_defs.h"

static void ordinary_string_limits_remain_catchable(struct kunit *test)
{
	static const char code[] = "local M={name='budget_combined',author='tests',"
		"description='combined',license='GPL-2.0',version=1};"
		"M.file_open=function()local ok,e=pcall(string.rep,'',1600000);"
		"assert(not ok and string.find(e,'string work limit exceeded',1,true));"
		"ok,e=pcall(string.match,'',string.rep('a*',65));"
		"assert(not ok and string.find(e,'pattern recursion limit exceeded',1,true));"
		"assert(string.match('ab','a?b')=='ab');return true end;return M\n";
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, sizeof(code)-1), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("budget_combined"), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
}
static struct kunit_case cases[] = {KUNIT_CASE(ordinary_string_limits_remain_catchable), {}};
static struct kunit_suite suite = {.name="lua-lsm-budget-combined", .init=lua_lsm_test_init, .test_cases=cases};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
