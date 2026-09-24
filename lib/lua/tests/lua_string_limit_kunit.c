// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

/* A private library copy permits small quotas; the normal library stays fixed. */
int luaopen_string_limited(lua_State *L);
#define LUA_STRING_WORK_TEST
#define luaopen_string luaopen_string_limited
#include "../lstrlib.c"
#undef luaopen_string

#define TRACE_SLOTS 8
struct charge_record {
	u64 initial, total, remaining, rejected_cost, rejected_remaining;
	enum StrWorkAPI api;
	bool finished, rejected;
};
struct charge_trace {
	struct charge_record records[TRACE_SLOTS];
	unsigned int used;
	u64 limit;
};
struct limit_fixture {
	lua_State *L;
	struct charge_trace trace;
};
static char trace_key;

static struct charge_trace *get_trace(lua_State *L)
{
	struct charge_trace *trace;

	lua_pushlightuserdata(L, &trace_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	trace = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return trace;
}

static void strwork_observer_begin(StrWork *work, enum StrWorkAPI api)
{
	struct charge_trace *trace = get_trace(work->L);
	struct charge_record *r;

	work->record = NULL;
	if (!trace)
		return;
	work->budget.remaining = trace->limit;
	if (trace->used == TRACE_SLOTS)
		return;
	r = &trace->records[trace->used++];
	memset(r, 0, sizeof(*r));
	r->api = api;
	r->initial = trace->limit;
	r->remaining = trace->limit;
	work->record = r;
}

static void strwork_observer_charge(StrWork *work, u64 cost, u64 before,
				    bool allowed)
{
	struct charge_record *r = work->record;

	if (!r)
		return;
	r->remaining = work->budget.remaining;
	if (allowed)
		r->total += cost;
	else {
		r->rejected = true;
		r->rejected_cost = cost;
		r->rejected_remaining = before;
	}
}

static void strwork_end(StrWork *work)
{
	struct charge_record *r = work->record;

	if (r)
		r->finished = true;
}

static void *lua_alloc(void *ud, void *ptr, size_t old, size_t size)
{
	if (!size) {
		kfree(ptr);
		return NULL;
	}
	return krealloc(ptr, size, GFP_KERNEL);
}

static void lua_close_action(void *data)
{
	lua_close(data);
}

static int open_libraries(lua_State *L)
{
	struct charge_trace *trace = lua_touserdata(L, 1);

	lua_pushcfunction(L, luaopen_base);
	lua_pushliteral(L, "");
	lua_call(L, 1, 0);
	lua_pushcfunction(L, luaopen_string_limited);
	lua_pushliteral(L, LUA_STRLIBNAME);
	lua_call(L, 1, 0);
	lua_pushlightuserdata(L, &trace_key);
	lua_pushlightuserdata(L, trace);
	lua_rawset(L, LUA_REGISTRYINDEX);
	return 0;
}

static int limit_init(struct kunit *test)
{
	struct limit_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	int status;

	if (!f)
		return -ENOMEM;
	f->L = lua_newstate(lua_alloc, NULL);
	if (!f->L)
		return -ENOMEM;
	status = kunit_add_action_or_reset(test, lua_close_action, f->L);
	if (status)
		return status;
	test->priv = f;
	f->trace.limit = LUA_STRING_WORK_LIMIT;
	lua_pushcfunction(f->L, open_libraries);
	lua_pushlightuserdata(f->L, &f->trace);
	if (lua_pcall(f->L, 1, 0, 0))
		return -EINVAL;
	return 0;
}

static void run(struct kunit *test, const char *chunk, int expected)
{
	struct limit_fixture *f = test->priv;
	int status;

	lua_settop(f->L, 0);
	memset(f->trace.records, 0, sizeof(f->trace.records));
	f->trace.used = 0;
	status = luaL_loadbuffer(f->L, chunk, strlen(chunk), "limit-test");
	KUNIT_ASSERT_EQ(test, status, 0);
	status = lua_pcall(f->L, 0, LUA_MULTRET, 0);
	KUNIT_ASSERT_EQ_MSG(test, status, expected, "%s: %s", chunk,
			   status ? lua_tostring(f->L, -1) : "success");
}

static struct charge_record *record(struct kunit *test, enum StrWorkAPI api,
				    bool rejected)
{
	struct limit_fixture *f = test->priv;
	struct charge_record *r;

	KUNIT_ASSERT_GT(test, f->trace.used, 0U);
	r = &f->trace.records[0];
	KUNIT_EXPECT_EQ(test, r->api, api);
	KUNIT_EXPECT_EQ(test, r->rejected, rejected);
	KUNIT_EXPECT_EQ(test, r->total +
		(rejected ? r->rejected_remaining : r->remaining), r->initial);
	if (rejected) {
		KUNIT_EXPECT_GT(test, r->rejected_cost, r->rejected_remaining);
		KUNIT_EXPECT_EQ(test, r->remaining, 0ULL);
		KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
						       "string work limit exceeded"));
	} else {
		KUNIT_EXPECT_TRUE(test, r->finished);
	}
	return r;
}

static void debit_test(struct kunit *test)
{
	StrWorkBudget b = { .remaining = 8 };

	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 7));
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 1));
	KUNIT_EXPECT_EQ(test, b.remaining, 0ULL);
	KUNIT_EXPECT_FALSE(test, strwork_debit(&b, 1));
	KUNIT_EXPECT_EQ(test, b.remaining, 0ULL);
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 0));
	b.remaining = ~(u64)0;
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, ~(u64)0));
	KUNIT_EXPECT_FALSE(test, strwork_debit(&b, ~(u64)0));
}

static void growth_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	run(test, "assert(string.match('a','a')=='a')", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->total, 9ULL);
	run(test, "assert(string.match('aaaa','^a?a?a?a?b')==nil)", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->total, 140ULL);
	run(test, "assert(string.match('aaaaaaaa','^(a+)%1b')==nil)", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->total, 84ULL);
	run(test, "assert(string.match('((((()))))','%b()')=='((((()))))')", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->total, 26ULL);
	run(test, "assert(string.find('aaaaaaaa','aaaaab',1,true)==nil)", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_FIND, false)->total, 22ULL);
	run(test, "assert(string.find('a\\000b','\\000b',1,true)==2)", 0);
	record(test, SW_FIND, false);
	run(test, "assert(string.match('xxxxxxxxxxxxxxxx','[aaaaaaaa]z')==nil)", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->total, 398ULL);
	KUNIT_EXPECT_LE(test, f->trace.used, (unsigned int)TRACE_SLOTS);
}

static void boundary_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;
	struct charge_record *r;
	const char *plain = "return string.find('aaaab','aaaac',1,true)";

	f->trace.limit = 8;
	run(test, "return string.match('a','a')", LUA_ERRRUN);
	record(test, SW_MATCH, true);
	f->trace.limit = 9;
	run(test, "assert(string.match('a','a')=='a')", 0);
	KUNIT_EXPECT_EQ(test, record(test, SW_MATCH, false)->remaining, 0ULL);
	f->trace.limit = 6;
	run(test, plain, LUA_ERRRUN);
	r = record(test, SW_FIND, true);
	KUNIT_EXPECT_EQ(test, r->rejected_cost, 4ULL);
	KUNIT_EXPECT_EQ(test, r->rejected_remaining, 3ULL);
	f->trace.limit = 7;
	run(test, plain, 0);
	KUNIT_EXPECT_TRUE(test, lua_isnil(f->L, -1));
	KUNIT_EXPECT_EQ(test, record(test, SW_FIND, false)->remaining, 0ULL);
	f->trace.limit = 3;
	run(test, "return string.gsub('abc','z','x',0)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, SW_GSUB, true)->rejected_cost, 3ULL);
	f->trace.limit = 4;
	run(test, "assert(string.gsub('abc','z','x',0)=='abc')", 0);
	record(test, SW_GSUB, false);
	f->trace.limit = 10;
	run(test, "hits=0; return string.gsub('a','a',function() hits=hits+1; return '12345678' end)", LUA_ERRRUN);
	lua_getglobal(f->L, "hits");
	KUNIT_EXPECT_EQ(test, lua_tointeger(f->L, -1), (lua_Integer)0);
	f->trace.limit = 11;
	run(test, "hits=0; return string.gsub('a','a',function() hits=hits+1; return '12345678' end)", LUA_ERRRUN);
	lua_getglobal(f->L, "hits");
	KUNIT_EXPECT_EQ(test, lua_tointeger(f->L, -1), (lua_Integer)1);
	f->trace.limit = 9;
	run(test, "for i=1,8 do assert(string.match('a','a')=='a') end; "
		 "return string.match('aaaa','^a?a?a?a?b')", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, f->trace.used, (unsigned int)TRACE_SLOTS);
}

static void depth_recovery_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	f->trace.limit = LUA_STRING_WORK_LIMIT;
	run(test, "local p=string.rep('()',32); assert(select('#',string.match('',p))==32)", 0);
	record(test, SW_MATCH, false);
	run(test, "return string.match('',string.rep('a*',65))", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
					       "pattern recursion limit exceeded"));
	run(test, "return string.match('x','[')", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
					       "malformed pattern"));
	f->trace.limit = 0;
	run(test, "return string.match({},'a')", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
					       "string expected"));
	f->trace.limit = 9;
	run(test, "assert(string.match('a','a')=='a')", 0);
	record(test, SW_MATCH, false);
}

static void iterator_reentry_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;
	lua_State *L = f->L;

	f->trace.limit = 8;
	run(test, "return string.gmatch('ab','.')", 0);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), LUA_ERRRUN);
	KUNIT_EXPECT_TRUE(test, f->trace.records[0].rejected);
	lua_settop(L, 1);
	f->trace.limit = 9;
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, -1), "a");
	lua_settop(L, 1);
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, -1), "b");
	f->trace.limit = LUA_STRING_WORK_LIMIT;
	run(test, "return string.gmatch(string.rep('a',32),string.rep('(a)',32))", 0);
	f->trace.limit = 323;
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, LUA_MULTRET, 0), LUA_ERRRUN);
	KUNIT_EXPECT_TRUE(test, f->trace.records[0].rejected);
	lua_settop(L, 1);
	f->trace.limit = 324;
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, LUA_MULTRET, 0), 0);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 33);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, 2), "a");
	f->trace.limit = 32;
	run(test, "assert(string.gsub('a','a',function(x) "
		 "assert(not pcall(string.match,'aaaa','^a?a?a?a?b')); return x end)=='a')", 0);
	KUNIT_ASSERT_EQ(test, f->trace.used, 2U);
	KUNIT_EXPECT_FALSE(test, f->trace.records[0].rejected);
	KUNIT_EXPECT_TRUE(test, f->trace.records[1].rejected);
}

static void production_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	lua_pushcfunction(f->L, luaopen_string);
	lua_pushliteral(f->L, LUA_STRLIBNAME);
	KUNIT_ASSERT_EQ(test, lua_pcall(f->L, 1, 0, 0), 0);
	f->trace.limit = 0; /* The real library cannot read the private override. */
	run(test, "assert(string.find(string.rep('a',65535),'z',1,true)==nil)", 0);
	run(test, "return string.find(string.rep('a',65536),'z',1,true)", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
					       "string work limit exceeded"));
	run(test, "assert(string.gsub('a','a','x')=='x')", 0);
	run(test, "return string.gsub(string.rep('a',65536),'z','x',0)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
}

static struct kunit_case limit_cases[] = {
	KUNIT_CASE(debit_test),
	KUNIT_CASE(growth_test),
	KUNIT_CASE(boundary_test),
	KUNIT_CASE(depth_recovery_test),
	KUNIT_CASE(iterator_reentry_test),
	KUNIT_CASE(production_test),
	{}
};

static struct kunit_suite limit_suite = {
	.name = "lua-string-limits",
	.init = limit_init,
	.test_cases = limit_cases,
};
kunit_test_suite(limit_suite);
MODULE_LICENSE("GPL");
