// SPDX-License-Identifier: GPL-2.0-only
/* Private copy changes only the clock and symbol names; no production observer. */
#include <kunit/test.h>
#include <linux/timekeeping.h>
#include <linux/lauxlib.h>
#include "lsm.h"

static u64 test_now;
static u64 test_clock(void) { return test_now; }
void test_budget_begin(lua_State *L, u64 timeout_ns);
int test_budget_end(lua_State *L);
#define ktime_get_ns test_clock
#define lua_budget_begin test_budget_begin
#define lua_budget_end test_budget_end
#define luaD_checkbudget test_checkbudget
#include "../../lib/lua/lbudget.c"
#undef ktime_get_ns

static void deadline_boundaries(struct kunit *test)
{
	lua_State *L = lvm_get();
	lua_State *co;
	u64 start = ktime_get_ns() + 10 * NSEC_PER_SEC;

	KUNIT_ASSERT_NOT_NULL(test, L);
	KUNIT_EXPECT_EQ(test, test_budget_end(L), 0);
	test_now = start;
	test_budget_begin(L, 100);
	test_now = start + 99;
	KUNIT_EXPECT_EQ(test, expired(G(L)), 0);
	test_now = start + 100;
	KUNIT_EXPECT_EQ(test, expired(G(L)), 1); /* equality expires */
	test_now = start; /* an expired budget stays expired */
	KUNIT_EXPECT_EQ(test, expired(G(L)), 1);
	KUNIT_EXPECT_EQ(test, test_budget_end(L), LUA_ERRTIME);
	KUNIT_EXPECT_FALSE(test, G(L)->budget_active);
	KUNIT_EXPECT_FALSE(test, G(L)->budget_expired);
	test_budget_begin(L, 0);
	KUNIT_EXPECT_EQ(test, test_budget_end(L), LUA_ERRTIME);
	test_budget_begin(L, 100);
	co = lua_newthread(L);
	KUNIT_EXPECT_PTR_EQ(test, G(co), G(L));
	KUNIT_EXPECT_EQ(test, G(co)->deadline, start + 100);
	test_now = start + 101;
	KUNIT_EXPECT_EQ(test, test_budget_end(co), LUA_ERRTIME);
	/* Signed subtraction handles the u64 wrap boundary too. */
	test_now = U64_MAX - 5;
	test_budget_begin(L, 10);
	test_now = 3;
	KUNIT_EXPECT_EQ(test, expired(G(L)), 0);
	test_now = 4;
	KUNIT_EXPECT_EQ(test, test_budget_end(L), LUA_ERRTIME);
	lua_settop(L, 0);
	lvm_put(L);
}

static int nested_begin(lua_State *L)
{
	test_budget_begin(L, 10000);
	return 0;
}
static void nested_budget_cannot_reset(struct kunit *test)
{
	lua_State *L = lvm_get();
	u64 start = ktime_get_ns() + 10 * NSEC_PER_SEC;

	test_now = start;
	test_budget_begin(L, 100);
	KUNIT_EXPECT_EQ(test, lua_cpcall(L, nested_begin, NULL), LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, G(L)->deadline, start + 100);
	KUNIT_EXPECT_EQ(test, test_budget_end(L), 0);
	lua_settop(L, 0);
	lvm_put(L);
}

static struct kunit_case cases[] = {
	KUNIT_CASE(deadline_boundaries),
	KUNIT_CASE(nested_budget_cannot_reset),
	{}
};
static struct kunit_suite suite = {.name="lua-lsm-budget-clock", .test_cases=cases};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
