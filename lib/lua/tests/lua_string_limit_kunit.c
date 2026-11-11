// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "../lstrwork.h"
#include "../lundump.h"

enum { DEFAULT_LIMIT = LUA_STRING_WORK_LIMIT };
static u64 test_limit(lua_State *L);
static bool test_debit(StrWorkBudget *budget, u64 cost);
int luaopen_string_limited(lua_State *L);

/* Instrument a private copy without adding hooks to production sources. */
#undef LUA_STRING_WORK_LIMIT
#define LUA_STRING_WORK_LIMIT test_limit(L)
#define strwork_debit test_debit
#define luaopen_string luaopen_string_limited
#include "../lstrlib.c"
#undef luaopen_string
#undef strwork_debit
#undef LUA_STRING_WORK_LIMIT

#define TRACE_SLOTS 8
struct charge_record {
	StrWorkBudget *budget;
	u64 total, remaining, rejected_cost, rejected_remaining;
	bool rejected;
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

static u64 test_limit(lua_State *L)
{
	struct charge_trace *trace = get_trace(L);

	return trace ? trace->limit : DEFAULT_LIMIT;
}

static bool test_debit(StrWorkBudget *budget, u64 cost)
{
	StrWork *work = container_of(budget, StrWork, budget);
	struct charge_trace *trace = get_trace(work->L);
	struct charge_record *r = NULL;
	u64 before = budget->remaining;
	bool allowed = strwork_debit(budget, cost);
	unsigned int i;

	if (!trace)
		return allowed;
	if (before == trace->limit) {
		if (trace->used < TRACE_SLOTS) {
			r = &trace->records[trace->used++];
			r->budget = budget;
		}
	} else {
		for (i = trace->used; i > 0; i--)
			if (trace->records[i - 1].budget == budget) {
				r = &trace->records[i - 1];
				break;
			}
	}
	if (r) {
		r->remaining = budget->remaining;
		if (allowed)
			r->total += cost;
		else {
			r->rejected = true;
			r->rejected_cost = cost;
			r->rejected_remaining = before;
		}
	}
	return allowed;
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
	f->trace.limit = DEFAULT_LIMIT;
	lua_pushcfunction(f->L, open_libraries);
	lua_pushlightuserdata(f->L, &f->trace);
	if (lua_pcall(f->L, 1, 0, 0))
		return -EINVAL;
	return 0;
}

static void reset(struct limit_fixture *f)
{
	lua_settop(f->L, 0);
	memset(f->trace.records, 0, sizeof(f->trace.records));
	f->trace.used = 0;
}

static void run(struct kunit *test, const char *chunk, int expected)
{
	struct limit_fixture *f = test->priv;
	int status;

	reset(f);
	status = luaL_loadbuffer(f->L, chunk, strlen(chunk), "limit-test");
	KUNIT_ASSERT_EQ(test, status, 0);
	status = lua_pcall(f->L, 0, LUA_MULTRET, 0);
	KUNIT_ASSERT_EQ_MSG(test, status, expected, "%s: %s", chunk,
			   status ? lua_tostring(f->L, -1) : "success");
}

static struct charge_record *record(struct kunit *test, bool rejected)
{
	struct limit_fixture *f = test->priv;
	struct charge_record *r;

	KUNIT_ASSERT_GT(test, f->trace.used, 0U);
	r = &f->trace.records[f->trace.used - 1];
	KUNIT_EXPECT_EQ(test, r->rejected, rejected);
	KUNIT_EXPECT_EQ(test, r->total +
		(rejected ? r->rejected_remaining : r->remaining), f->trace.limit);
	if (rejected) {
		KUNIT_EXPECT_GT(test, r->rejected_cost, r->rejected_remaining);
		KUNIT_EXPECT_EQ(test, r->remaining, 0ULL);
		KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1),
						       "string work limit exceeded"));
	}
	return r;
}

static void boundary(struct kunit *test, const char *chunk, u64 cost)
{
	struct limit_fixture *f = test->priv;

	f->trace.limit = cost - 1;
	run(test, chunk, LUA_ERRRUN);
	record(test, true);
	f->trace.limit = cost;
	run(test, chunk, 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->remaining, 0ULL);
}

static void set_blob(struct kunit *test, const char *name, size_t len, char c)
{
	struct limit_fixture *f = test->priv;
	char *data = kunit_kmalloc(test, len ? len : 1, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, data);
	memset(data, c, len);
	lua_pushlstring(f->L, data, len);
	lua_setglobal(f->L, name);
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
	boundary(test, "assert(string.match('a','a')=='a')", 9);
	boundary(test, "assert(string.match('aaaa','^a?a?a?a?b')==nil)", 140);
	boundary(test, "assert(string.match('aaaaaaaa','^(a+)%1b')==nil)", 84);
	boundary(test, "assert(string.match('((((()))))','%b()')=='((((()))))')", 26);
	boundary(test, "assert(string.find('aaaaaaaa','aaaaab',1,true)==nil)", 22);
	boundary(test, "assert(string.find('a\\000b','\\000b',1,true)==2)", 5);
	boundary(test, "assert(string.match('xxxxxxxxxxxxxxxx','[aaaaaaaa]z')==nil)", 398);
}

static void precharge_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	boundary(test, "return string.find('aaaab','aaaac',1,true)", 7);
	f->trace.limit = 6;
	run(test, "return string.find('aaaab','aaaac',1,true)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 4ULL);
	boundary(test, "assert(string.gsub('abc','z','x',0)=='abc')", 4);
	f->trace.limit = 10;
	run(test, "hits=0; return string.gsub('a','a',function() hits=hits+1; return '12345678' end)", LUA_ERRRUN);
	lua_getglobal(f->L, "hits");
	KUNIT_EXPECT_EQ(test, lua_tointeger(f->L, -1), (lua_Integer)0);
	f->trace.limit = 11;
	run(test, "hits=0; return string.gsub('a','a',function() hits=hits+1; return '12345678' end)", LUA_ERRRUN);
	lua_getglobal(f->L, "hits");
	KUNIT_EXPECT_EQ(test, lua_tointeger(f->L, -1), (lua_Integer)1);
	f->trace.limit = 9;
	run(test, "for i=1,8 do assert(string.match('a','a')=='a') end; return string.match('aaaa','^a?a?a?a?b')", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, f->trace.used, (unsigned int)TRACE_SLOTS);
}

static void depth_recovery_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	run(test, "p32=string.rep('()',32); p66=string.rep('a*',65)", 0);
	run(test, "assert(select('#',string.match('',p32))==32)", 0);
	run(test, "return string.match('',p66)", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "pattern recursion limit exceeded"));
	run(test, "return string.match('x','[')", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "malformed pattern"));
	f->trace.limit = 0;
	run(test, "return string.match({},'a')", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
	f->trace.limit = 9;
	run(test, "assert(string.match('a','a')=='a')", 0);
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
	lua_settop(L, 1);
	f->trace.limit = 9;
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, -1), "a");
	lua_settop(L, 1);
	lua_pushvalue(L, 1);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, -1), "b");
	f->trace.limit = DEFAULT_LIMIT;
	run(test, "iter=string.gmatch(string.rep('a',32),string.rep('(a)',32))", 0);
	f->trace.limit = 323;
	run(test, "return iter()", LUA_ERRRUN);
	f->trace.limit = 324;
	run(test, "return iter()", 0);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 32);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, 1), "a");
	f->trace.limit = 32;
	run(test, "assert(string.gsub('a','a',function(x) assert(not pcall(string.match,'aaaa','^a?a?a?a?b')); return x end)=='a')", 0);
	KUNIT_ASSERT_EQ(test, f->trace.used, 2U);
	KUNIT_EXPECT_FALSE(test, f->trace.records[0].rejected);
	KUNIT_EXPECT_TRUE(test, f->trace.records[1].rejected);
}

static void substring_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	boundary(test, "assert(string.sub('abcdef',2,4)=='bcd')", 4);
	boundary(test, "assert(string.sub('a\\000b',2,3)=='\\000b')", 3);
	boundary(test, "assert(string.sub('abcdef',-2)=='ef')", 3);
	boundary(test, "assert(string.sub('abc',3,1)=='')", 1);
	set_blob(test, "large", 65536, 'a');
	f->trace.limit = 2;
	run(test, "assert(string.sub(large,1,1)=='a')", 0);
	f->trace.limit = 65536;
	run(test, "return string.sub(large,1)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 65536ULL);
	f->trace.limit = 0;
	run(test, "assert(string.len(large)==65536)", 0);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
}

static void transform_test(struct kunit *test)
{
	boundary(test, "assert(string.lower('Ab\\000C')=='ab\\000c')", 5);
	boundary(test, "assert(string.upper('Ab\\000C')=='AB\\000C')", 5);
	boundary(test, "assert(string.reverse('Ab\\000C')=='C\\000bA')", 5);
	boundary(test, "assert(string.lower('')=='')", 1);
	boundary(test, "assert(string.reverse('')=='')", 1);
}

static void repetition_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	boundary(test, "assert(string.rep('ab',3)=='ababab')", 10);
	boundary(test, "assert(string.rep('',4)=='')", 5);
	boundary(test, "assert(string.rep('ab',0)=='')", 1);
	boundary(test, "assert(string.rep('ab',-2147483648)=='')", 1);
	f->trace.limit = 65536;
	run(test, "return string.rep('',2147483647)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 2147483647ULL);
	set_blob(test, "large", 65536, 'a');
	run(test, "return string.rep(large,2)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 65536ULL);
	run(test, "assert(string.rep('',65535)=='')", 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->remaining, 0ULL);
}

static void byte_char_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	boundary(test, "local a,b,c=string.byte('a\\000b',1,3); assert(a==97 and b==0 and c==98)", 4);
	boundary(test, "assert(string.byte('abc',-1)==99)", 2);
	boundary(test, "assert(select('#',string.byte('',1))==0)", 1);
	boundary(test, "assert(string.char(65,0,66)=='A\\000B')", 4);
	boundary(test, "assert(string.char('65',0,66)=='A\\000B')", 6);
	boundary(test, "assert(string.char()=='')", 1);
	set_blob(test, "large", 8001, 'a');
	f->trace.limit = DEFAULT_LIMIT;
	run(test, "return string.byte(large,1,8001)", LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "string slice too long"));
	run(test, "return string.char(256)", LUA_ERRRUN);
	run(test, "assert(string.char(65)=='A')", 0);
}

static void format_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	lua_pushinteger(f->L, 4294967297LL);
	lua_setglobal(f->L, "format_wide");
	lua_pushinteger(f->L, -9223372036854775807LL - 1);
	lua_setglobal(f->L, "format_min");
	run(test, "assert(string.format('%d:%i:%x:%X:%o:%u',"
	    "format_wide,format_wide,format_wide,format_wide,format_wide,format_wide)=="
	    "'4294967297:4294967297:100000001:100000001:40000000001:4294967297')", 0);
	run(test, "assert(string.format('%d',format_min)=='-9223372036854775808');"
	    "assert(string.format('%u',-1)=='18446744073709551615');"
	    "assert(string.format('%d',-format_wide)=='-4294967297')", 0);
	boundary(test, "assert(string.format('abc')=='abc')", 7);
	boundary(test, "assert(string.format('%%')=='%')", 4);
	boundary(test, "assert(string.format('%d',42)=='42')", 5);
	boundary(test, "assert(string.format('%04d',42)=='0042')", 9);
	boundary(test, "assert(string.format('%s','abc')=='abc')", 9);
	boundary(test, "assert(string.format('%q','\\000')=='\"\\\\000\"')", 10);
	boundary(test, "assert(string.format('%q','a\\000b')=='\"a\\\\000b\"')", 14);
	boundary(test, "assert(string.format('%d','123')=='123')", 9);
	boundary(test, "assert(string.format('')=='')", 1);
	set_blob(test, "large", 65536, 'a');
	f->trace.limit = 7;
	run(test, "assert(string.format('%.1s',large)=='a')", 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->remaining, 0ULL);
	f->trace.limit = 65536;
	run(test, "return string.format('%q',large)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 65536ULL);
	run(test, "return string.format('%.s',large)", LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, 65536ULL);
	run(test, "return string.format('%s',large)", LUA_ERRRUN);
	run(test, "return string.format(large)", LUA_ERRRUN);
	run(test, "return string.format('%999d',1)", LUA_ERRRUN);
	run(test, "return string.format('%p',1)", LUA_ERRRUN);
	run(test, "assert(string.format('%x:%c',255,65)=='ff:A')", 0);
}

static void numeric_argument_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	boundary(test, "assert(string.sub('abc','2','3')=='bc')", 5);
	boundary(test, "assert(string.rep('a','2')=='aa')", 6);
	boundary(test, "assert(string.find('abc','b','1',true)==2)", 5);
	set_blob(test, "digits", 65536, '0');
	f->trace.limit = 65536;
	run(test, "return string.sub('a',digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.rep('a',digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.byte('a',digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.char(digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.format('%d',digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.find('a','a',digits)", LUA_ERRRUN);
	record(test, true);
	run(test, "return string.gsub('a','a','b',digits)", LUA_ERRRUN);
	record(test, true);
}

static int dump_tree(lua_State *L)
{
	StrDump dump;

	strwork_begin(L, &dump.work);
	luaL_buffinit(L, &dump.buffer);
	if (luaU_dump(L, lua_touserdata(L, 1), writer, &dump, 0))
		return luaL_error(L, "unable to dump given function");
	luaL_pushresult(&dump.buffer);
	return 1;
}

static void call_dump(struct kunit *test, Proto *p, int expected)
{
	struct limit_fixture *f = test->priv;

	reset(f);
	lua_pushcfunction(f->L, dump_tree);
	lua_pushlightuserdata(f->L, p);
	KUNIT_ASSERT_EQ(test, lua_pcall(f->L, 1, 1, 0), expected);
}

static void dump_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;
	Proto *p = kunit_kcalloc(test, 33, sizeof(*p), GFP_KERNEL);
	Proto **links = kunit_kcalloc(test, 33, sizeof(*links), GFP_KERNEL);
	unsigned int i;
	/* DumpFunction: source size, eight ints, four chars; 15 writer calls. */
	size_t proto_bytes = sizeof(size_t) + 8 * sizeof(int) + 4 * sizeof(char);
	size_t dump_bytes = LUAC_HEADERSIZE + 32 * proto_bytes;
	/* Initial charge, then one unit per writer call plus its byte count. */
	u64 cost = 1 + (LUAC_HEADERSIZE + 1) + 32 * (proto_bytes + 15);

	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_NOT_NULL(test, links);
	for (i = 0; i < 31; i++) {
		links[i] = &p[i + 1];
		p[i].p = &links[i];
		p[i].sizep = 1;
	}
	f->trace.limit = cost - 1;
	call_dump(test, p, LUA_ERRRUN);
	record(test, true);
	f->trace.limit = cost;
	call_dump(test, p, 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->remaining, 0ULL);
	KUNIT_EXPECT_EQ(test, lua_objlen(f->L, -1), dump_bytes);
	links[31] = &p[32];
	p[31].p = &links[31];
	p[31].sizep = 1;
	f->trace.limit = DEFAULT_LIMIT;
	call_dump(test, p, LUA_ERRRUN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "unable to dump"));
	p[31].sizep = 0;
	call_dump(test, p, 0);
	p[0].sizep = 0;
	p[0].code = kunit_kzalloc(test, DEFAULT_LIMIT, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p[0].code);
	p[0].sizecode = DEFAULT_LIMIT / sizeof(Instruction);
	call_dump(test, p, LUA_ERRRUN);
	KUNIT_EXPECT_EQ(test, record(test, true)->rejected_cost, (u64)DEFAULT_LIMIT);
	p[0].sizecode = 0;
	call_dump(test, p, 0);
	run(test, "local f=function(x) return x+1 end; local b=string.dump(f); assert(loadstring(b)(41)==42)", 0);
	run(test, "return string.dump(string.len)", LUA_ERRRUN);
	run(test, "assert(type(string.dump(function() return 1 end))=='string')", 0);
}

static void production_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;

	set_blob(test, "at_limit", DEFAULT_LIMIT - 1, 'a');
	set_blob(test, "over_limit", DEFAULT_LIMIT, 'a');
	lua_pushinteger(f->L, DEFAULT_LIMIT);
	lua_setglobal(f->L, "work_limit");
	lua_pushcfunction(f->L, luaopen_string);
	lua_pushliteral(f->L, LUA_STRLIBNAME);
	KUNIT_ASSERT_EQ(test, lua_pcall(f->L, 1, 0, 0), 0);
	f->trace.limit = 0;
	run(test, "assert(string.find(at_limit,'z',1,true)==nil)", 0);
	run(test, "return string.find(over_limit,'z',1,true)", LUA_ERRRUN);
	run(test, "assert(string.sub(at_limit,1)==at_limit)", 0);
	run(test, "return string.sub(over_limit,1)", LUA_ERRRUN);
	run(test, "assert(#string.lower(at_limit)==work_limit-1)", 0);
	run(test, "return string.upper(over_limit)", LUA_ERRRUN);
	run(test, "return string.reverse(over_limit)", LUA_ERRRUN);
	run(test, "assert(string.rep('a',(work_limit-1)/2)==string.sub(at_limit,1,(work_limit-1)/2))", 0);
	run(test, "return string.rep('a',work_limit/2)", LUA_ERRRUN);
	run(test, "return string.rep('',2147483647)", LUA_ERRRUN);
	run(test, "assert(string.format('%.1s',over_limit)=='a')", 0);
	run(test, "return string.format('%q',over_limit)", LUA_ERRRUN);
	run(test, "assert(string.len(over_limit)==work_limit)", 0);
	KUNIT_EXPECT_EQ(test, f->trace.used, 0U);
}

static void corpus_test(struct kunit *test)
{
	struct limit_fixture *f = test->priv;
	static const struct {
		const char *name, *chunk;
		u64 cost;
	} cases[] = {
		{ "path-sub", "assert(string.sub('/usr/lib/modules',6,8)=='lib')", 4 },
		{ "path-lower", "assert(string.lower('/USR/LIB/MODULES')=='/usr/lib/modules')", 17 },
		{ "path-upper", "assert(string.upper('/usr/lib/modules')=='/USR/LIB/MODULES')", 17 },
		{ "reverse-4k", "assert(#string.reverse(page)==4096)", 4097 },
		{ "lower-4k", "assert(#string.lower(page)==4096)", 4097 },
		{ "upper-4k", "assert(#string.upper(page)==4096)", 4097 },
		{ "sub-4k", "assert(string.sub(page,1)==page)", 4097 },
		{ "repeat-4k", "assert(#string.rep('ab',2048)==4096)", 6145 },
		{ "format-4k", "assert(string.format('%s',page)==page)", 8195 },
		{ "quote-4k", "assert(#string.format('%q',page)==4098)", 8197 },
		{ "format-precision", "assert(string.format('%.16s',page)=='aaaaaaaaaaaaaaaa')", 38 },
		{ "numeric-format", "assert(string.format('uid=%d mode=%04x',1000,420)=='uid=1000 mode=01a4')", 35 },
	};
	unsigned int i;

	set_blob(test, "page", 4096, 'a');
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		run(test, cases[i].chunk, 0);
		KUNIT_EXPECT_EQ_MSG(test, record(test, false)->total, cases[i].cost, "%s", cases[i].name);
		kunit_info(test, "STRING_CORPUS %s work=%llu", cases[i].name,
			   record(test, false)->total);
	}
	f->trace.limit = DEFAULT_LIMIT;
	set_blob(test, "xattr_value", 65536, 'a');
	run(test, "assert(#string.match(xattr_value,'^a*$')==65536)", 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->total, 131082ULL);
	run(test, "assert(#string.lower(xattr_value)==65536)", 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->total, 65537ULL);
	run(test, "path='/'..string.rep(string.rep('!',255)..'/',15)..string.rep('!',254); assert(#path==4095)", 0);
	run(test, "local s,n=string.gsub(path,'(%W)','%%%1'); assert(n==4095 and #s==8190)", 0);
	KUNIT_EXPECT_EQ(test, record(test, false)->total, 86003ULL);
}

static struct kunit_case limit_cases[] = {
	KUNIT_CASE(debit_test),
	KUNIT_CASE(growth_test),
	KUNIT_CASE(precharge_test),
	KUNIT_CASE(depth_recovery_test),
	KUNIT_CASE(iterator_reentry_test),
	KUNIT_CASE(substring_test),
	KUNIT_CASE(transform_test),
	KUNIT_CASE(repetition_test),
	KUNIT_CASE(byte_char_test),
	KUNIT_CASE(format_test),
	KUNIT_CASE(numeric_argument_test),
	KUNIT_CASE(dump_test),
	KUNIT_CASE(production_test),
	KUNIT_CASE(corpus_test),
	{}
};

static struct kunit_suite limit_suite = {
	.name = "lua-string-limits",
	.init = limit_init,
	.test_cases = limit_cases,
};
kunit_test_suite(limit_suite);
MODULE_LICENSE("GPL");
