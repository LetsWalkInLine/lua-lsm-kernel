// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lua_work_test.h"

struct work_fixture {
	lua_State *L;
	struct lua_work_observer observer;
};

static void *work_alloc(void *ud, void *ptr, size_t old, size_t size)
{
	if (!size) {
		kfree(ptr);
		return NULL;
	}
	return krealloc(ptr, size, GFP_KERNEL);
}

static void work_close(void *data)
{
	lua_close(data);
}

static int work_open(lua_State *L)
{
	struct lua_work_observer *observer = lua_touserdata(L, 1);

	lua_pushcfunction(L, luaopen_base);
	lua_pushliteral(L, "");
	lua_call(L, 1, 0);
	lua_pushcfunction(L, luaopen_string_work);
	lua_pushliteral(L, LUA_STRLIBNAME);
	lua_call(L, 1, 0);
	lua_work_observe(L, observer);
	return 0;
}

static int work_init(struct kunit *test)
{
	struct work_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	int status;

	if (!f)
		return -ENOMEM;
	f->L = lua_newstate(work_alloc, NULL);
	if (!f->L)
		return -ENOMEM;
	status = kunit_add_action_or_reset(test, work_close, f->L);
	if (status)
		return status;
	test->priv = f;
	lua_pushcfunction(f->L, work_open);
	lua_pushlightuserdata(f->L, &f->observer);
	status = lua_pcall(f->L, 1, 0, 0);
	if (status) {
		kunit_err(test, "opening work library: %s\n", lua_tostring(f->L, -1));
		return -EINVAL;
	}
	return 0;
}

static void work_run(struct kunit *test, const char *chunk, int expected_status)
{
	struct work_fixture *f = test->priv;
	int status;

	lua_settop(f->L, 0);
	status = luaL_loadbuffer(f->L, chunk, strlen(chunk), "work-test");
	KUNIT_ASSERT_EQ_MSG(test, status, 0, "loading %s: %s", chunk,
			   status ? lua_tostring(f->L, -1) : "success");
	memset(&f->observer, 0, sizeof(f->observer));
	status = lua_pcall(f->L, 0, LUA_MULTRET, 0);
	KUNIT_ASSERT_EQ_MSG(test, status, expected_status, "%s: %s", chunk,
			   status ? lua_tostring(f->L, -1) : "success");
	KUNIT_ASSERT_FALSE(test, f->observer.full);
}

static struct lua_work_record *work_record(struct kunit *test, unsigned int i,
					 enum StrWorkAPI api, bool finished)
{
	struct work_fixture *f = test->priv;
	struct lua_work_record *r;
	u64 total = 0;
	int k;

	KUNIT_ASSERT_LT(test, i, f->observer.used);
	r = &f->observer.records[i];
	KUNIT_EXPECT_EQ(test, r->api, api);
	KUNIT_EXPECT_EQ(test, r->finished, finished);
	KUNIT_EXPECT_FALSE(test, r->overflow);
	KUNIT_EXPECT_EQ(test, r->count[SW_OPERATION], 1ULL);
	for (k = 0; k < SW_KINDS; k++)
		total += r->count[k];
	KUNIT_EXPECT_EQ(test, total, r->total);
	return r;
}

/* Arguments and binary strings are prepared before the measured C call. */
static struct lua_work_record *work_find(struct kunit *test,
		const char *src, size_t len, const char *pat, size_t plen,
		bool plain, int init, int expected_start)
{
	struct work_fixture *f = test->priv;
	lua_State *L = f->L;
	int status;

	lua_settop(L, 0);
	lua_getglobal(L, "string");
	lua_getfield(L, -1, "find");
	lua_remove(L, -2);
	lua_pushlstring(L, src, len);
	lua_pushlstring(L, pat, plen);
	lua_pushinteger(L, init);
	lua_pushboolean(L, plain);
	memset(&f->observer, 0, sizeof(f->observer));
	status = lua_pcall(L, 4, LUA_MULTRET, 0);
	KUNIT_ASSERT_EQ(test, status, 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 1U);
	if (expected_start) {
		KUNIT_ASSERT_GE(test, lua_gettop(L), 2);
		KUNIT_EXPECT_EQ(test, lua_tointeger(L, 1), (lua_Integer)expected_start);
		KUNIT_EXPECT_EQ(test, lua_tointeger(L, 2),
			       (lua_Integer)(expected_start + plen - 1));
	} else {
		KUNIT_EXPECT_EQ(test, lua_gettop(L), 1);
		KUNIT_EXPECT_TRUE(test, lua_isnil(L, 1));
	}
	return work_record(test, 0, SW_FIND, true);
}

static void work_plain_test(struct kunit *test)
{
	struct lua_work_record *r;
	char src[66], pat[66];
	int n;

	r = work_find(test, "xx/secret", 9, "/secret", 7, true, 1, 3);
	KUNIT_EXPECT_EQ(test, r->total, 11ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_CLASSIFY], 0ULL);
	for (n = 8; n <= 16; n += 4) {
		memset(src, 'a', n);
		r = work_find(test, src, n, "aaaaab", 6, true, 1, 0);
		KUNIT_EXPECT_EQ(test, r->count[SW_PLAIN_SCAN], (u64)n - 5);
		KUNIT_EXPECT_EQ(test, r->count[SW_PLAIN_CANDIDATE], (u64)n - 5);
		KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], 5ULL * (n - 5));
		KUNIT_EXPECT_EQ(test, r->total, 1ULL + 7 * (n - 5));
	}
	for (n = 8; n <= 32; n *= 2) {
		memset(src, 'a', n);
		memset(pat, 'a', n);
		src[n] = 'c';
		pat[n] = 'b';
		r = work_find(test, src, n + 1, pat, n + 1, true, 1, 0);
		KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], (u64)n);
		KUNIT_EXPECT_EQ(test, r->total, (u64)n + 3);
		/* Earlier memcmp failure pays the same requested length. */
		src[1] = 'x';
		r = work_find(test, src, n + 1, pat, n + 1, true, 1, 0);
		KUNIT_EXPECT_EQ(test, r->total, (u64)n + 3);
	}
	for (n = 8; n <= 64; n *= 2) {
		memset(pat, 'a', n);
		r = work_find(test, "x", 1, pat, n, false, 1, 0);
		KUNIT_EXPECT_EQ(test, r->count[SW_CLASSIFY], (u64)n + 1);
		KUNIT_EXPECT_EQ(test, r->total, (u64)n + 2);
	}
	kunit_info(test, "P01=11 P02(n=16)=78 P03(k=32)=35 P04(p=64)=66\n");
}

static void work_binary_test(struct kunit *test)
{
	struct lua_work_record *r;

	r = work_find(test, "xa\0b", 4, "a\0b", 3, true, 1, 2);
	KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], 2ULL);
	r = work_find(test, "x\0.y", 4, "\0.", 2, false, 1, 2);
	KUNIT_EXPECT_EQ(test, r->count[SW_CLASSIFY], 1ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], 1ULL);
	r = work_find(test, "", 0, "", 0, true, 1, 1);
	KUNIT_EXPECT_EQ(test, r->total, 1ULL);
	r = work_find(test, "a", 1, "aa", 2, true, 1, 0);
	KUNIT_EXPECT_EQ(test, r->total, 1ULL);
	r = work_find(test, "xxa", 3, "a", 1, true, 1, 3);
	KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], 0ULL);
	r = work_find(test, "xxa", 3, "a", 1, true, -1, 3);
	KUNIT_EXPECT_EQ(test, r->count[SW_PLAIN_SCAN], 1ULL);
	r = work_find(test, "xxa", 3, "a", 1, true, 99, 0);
	KUNIT_EXPECT_EQ(test, r->count[SW_PLAIN_SCAN], 0ULL);
	r = work_find(test, "a", 1, "a", 1, true, -99, 1);
	KUNIT_EXPECT_EQ(test, r->count[SW_PLAIN_SCAN], 1ULL);
	work_run(test, "assert(string.find('a', '.') == 1)", 0);
	r = work_record(test, 0, SW_FIND, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_CLASSIFY], 1ULL);
	work_run(test, "assert(string.find('aa', 'a.') == 1)", 0);
	r = work_record(test, 0, SW_FIND, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_CLASSIFY], 2ULL);
}

static void work_match_test(struct kunit *test)
{
	struct lua_work_record *r;
	struct lua_work_record saved;
	char src[34], pat[40], chunk[160];
	u64 previous[2] = { 0, 0 };
	int n, i, kind;

	work_run(test, "assert(string.match('a', 'a') == 'a')", 0);
	r = work_record(test, 0, SW_MATCH, true);
	KUNIT_EXPECT_EQ(test, r->total, 9ULL);
	saved = *r;
	work_run(test, "assert(string.match('a', 'a') == 'a')", 0);
	r = work_record(test, 0, SW_MATCH, true);
	KUNIT_EXPECT_MEMEQ(test, r, &saved, sizeof(saved));
	work_run(test, "assert(string.match('', '^$') == '')", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->total, 5ULL);
	work_run(test, "assert(string.match('abc', '^b') == nil)", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->total, 6ULL);
	for (n = 2; n <= 8; n *= 2) {
		memset(src, 'a', n);
		src[n] = '\0';
		pat[0] = '^';
		for (i = 0; i < n; i++) {
			pat[1 + 2 * i] = 'a';
			pat[2 + 2 * i] = '?';
		}
		pat[1 + 2 * n] = 'b';
		pat[2 + 2 * n] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '%s') == nil)", src, pat);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_START], 1ULL);
		KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], 1ULL << n);
		KUNIT_EXPECT_EQ(test, r->count[SW_RETRY], 2ULL * ((1 << n) - 1));
		src[n] = 'b';
		src[n + 1] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '%s') == '%s')", src, pat, src);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], (u64)n + 1);
		KUNIT_EXPECT_EQ(test, r->count[SW_RETRY], (u64)n);
	}
	for (n = 4; n <= 16; n *= 2) {
		memset(src, 'a', n);
		src[n] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', 'aaab') == nil)", src);
		work_run(test, chunk, 0);
		KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->count[SW_START], (u64)n + 1);
		for (kind = 0; kind < 2; kind++) {
			snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '%s') == nil)",
				 src, kind ? "^a-a-a-b" : "^a*a*a*b");
			work_run(test, chunk, 0);
			r = work_record(test, 0, SW_MATCH, true);
			KUNIT_EXPECT_GT(test, r->count[SW_RETRY], previous[kind]);
			KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], r->count[SW_RETRY] + 1);
			previous[kind] = r->count[SW_RETRY];
		}
	}
	kunit_info(test, "literal match=9 R02(n=8): enter=256 retry=510\n");
}

static void work_scan_test(struct kunit *test)
{
	struct lua_work_record *r;
	char members[65], chunk[192], src[35];
	int c, n;

	for (c = 8; c <= 64; c *= 2) {
		memset(members, 'a', c);
		members[c] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('xxxxxxxxxxxxxxxx', '[%s]z') == nil)", members);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_PATTERN], 17ULL * (c + 2));
		KUNIT_EXPECT_EQ(test, r->count[SW_CLASS], 16ULL * (c + 2));
		snprintf(chunk, sizeof(chunk), "assert(string.match('xxxxxxxxxxxxxxxx', '%%f[%s]z') == nil)", members);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_PATTERN], 17ULL * (c + 2));
		KUNIT_EXPECT_EQ(test, r->count[SW_CLASS], 34ULL * (c + 2));
		KUNIT_EXPECT_EQ(test, r->count[SW_SUBJECT], 34ULL);
	}
	work_run(test, "assert(string.match('b', '[%%a-cx]') == 'b')", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->count[SW_CLASS], 6ULL);
	work_run(test, "assert(string.match('aa', '^%f[a]', 2) == nil)", 0);
	r = work_record(test, 0, SW_MATCH, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_SUBJECT], 1ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_CLASS], 2ULL);
	for (n = 4; n <= 16; n *= 2) {
		memset(src, '(', n + 1);
		memset(src + n + 1, ')', n + 1);
		src[2 * n + 2] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '%%b()') == '%s')", src, src);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_SUBJECT], 2ULL * n + 2);
		KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], 1ULL);
		src[2 * n + 1] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '^%%b()$') == nil)", src);
		work_run(test, chunk, 0);
		KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->count[SW_SUBJECT], 2ULL * n + 1);
	}
}

static void work_capture_test(struct kunit *test)
{
	struct lua_work_record *r;
	char src[33], chunk[128];
	int n;

	for (n = 8; n <= 32; n *= 2) {
		memset(src, 'a', n);
		src[n] = '\0';
		snprintf(chunk, sizeof(chunk), "assert(string.match('%s', '^(a+)%%1b') == nil)", src);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_MATCH, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_COMPARE], (u64)(n / 2) * (n / 2 + 1) / 2);
		KUNIT_EXPECT_GT(test, r->count[SW_CAPTURE_SLOT], 0ULL);
	}
	work_run(test, "assert(string.match('ab', '(a)(b)') == 'a')", 0);
	r = work_record(test, 0, SW_MATCH, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_CAPTURE_SLOT], 2ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_CAPTURE_EMIT], 2ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 2ULL);
	work_run(test, "assert(string.match('', '()') == 1)", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_MATCH, true)->count[SW_OUTPUT], 0ULL);
}

static void work_iterator_test(struct kunit *test)
{
	struct work_fixture *f = test->priv;
	struct lua_work_record saved;
	unsigned int i;

	work_run(test, "local i=string.gmatch('aaaaaaaaaaaaaaaab','b');"
		 "assert(i()=='b'); assert(select('#',i())==0)", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 2U);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_GMATCH, true)->count[SW_START], 17ULL);
	KUNIT_EXPECT_EQ(test, work_record(test, 1, SW_GMATCH, true)->count[SW_START], 1ULL);
	work_run(test, "local i=string.gmatch('ab','()'); assert(i()==1);"
		 "assert(i()==2); assert(i()==3); assert(select('#',i())==0)", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 4U);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, work_record(test, i, SW_GMATCH, true)->count[SW_START], 1ULL);
	KUNIT_EXPECT_EQ(test, work_record(test, 3, SW_GMATCH, true)->total, 1ULL);
	work_run(test, "assert(string.gmatch('a','.')()=='a')", 0);
	saved = *work_record(test, 0, SW_GMATCH, true);
	work_run(test, "assert(string.gfind('a','.')()=='a')", 0);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 0, SW_GMATCH, true), &saved, sizeof(saved));
}

static void work_output_test(struct kunit *test)
{
	struct lua_work_record *r;
	char chunk[192];
	int ref, repeats;

	for (ref = 0; ref <= 1; ref++) {
		for (repeats = 1; repeats <= 2; repeats++) {
			snprintf(chunk, sizeof(chunk),
				 "local s,n=string.gsub('aaaa','%s','%%%d%s');"
				 "assert(s=='%s' and n==4)", ref ? "(a)" : "a", ref,
				 repeats == 2 ? (ref ? "%1" : "%0") : "",
				 repeats == 2 ? "aaaaaaaa" : "aaaa");
			work_run(test, chunk, 0);
			r = work_record(test, 0, SW_GSUB, true);
			KUNIT_EXPECT_EQ(test, r->count[SW_REPLACEMENT_SCAN], 8ULL * repeats);
			KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 4ULL * repeats * (ref + 1));
			KUNIT_EXPECT_EQ(test, r->count[SW_REPLACEMENT], 4ULL);
		}
	}
	work_run(test, "assert(string.gsub('aaaa','a','x',2)=='xxaa')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_START], 2ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 4ULL);
	for (ref = -1; ref <= 0; ref++) {
		snprintf(chunk, sizeof(chunk), "assert(string.gsub('aaaa','a','x',%d)=='aaaa')", ref);
		work_run(test, chunk, 0);
		r = work_record(test, 0, SW_GSUB, true);
		KUNIT_EXPECT_EQ(test, r->count[SW_START], 0ULL);
		KUNIT_EXPECT_EQ(test, r->total, 5ULL);
	}
	work_run(test, "assert(string.gsub('aaaaaaaaaaaaaaaa','b','x')=='aaaaaaaaaaaaaaaa')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_START], 17ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 16ULL);
	work_run(test, "assert(string.gsub('ab','()','-')=='-a-b-')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_START], 3ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 5ULL);
	work_run(test, "assert(string.gsub('a','a',function() return false end)=='a')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 3ULL); /* argument, fallback, append */
	work_run(test, "assert(string.gsub('a','a',function() return 123 end)=='123')", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_GSUB, true)->count[SW_OUTPUT], 4ULL);
	work_run(test, "assert(string.gsub('a','a',123)=='123')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_REPLACEMENT_SCAN], 3ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 3ULL);
	work_run(test, "assert(string.gsub('a','a','%')=='\\000')", 0);
	r = work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, r->count[SW_REPLACEMENT_SCAN], 2ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 1ULL);
}

static void work_reentry_test(struct kunit *test)
{
	struct work_fixture *f = test->priv;
	struct lua_work_record outer;
	u64 inner;

	work_run(test, "assert(string.gsub('a1b2','%d',function(x)"
		 "return '<'..string.match(x,'%d')..'>' end)=='a<1>b<2>')", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 3U);
	outer = *work_record(test, 0, SW_GSUB, true);
	KUNIT_EXPECT_EQ(test, outer.count[SW_CALLBACK], 2ULL);
	inner = work_record(test, 1, SW_MATCH, true)->total;
	work_record(test, 2, SW_MATCH, true);
	work_run(test, "assert(string.gsub('a1b2','%d',function(x)"
		 "return '<'..string.match(x,'%d?%d?%d')..'>' end)=='a<1>b<2>')", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 3U);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 0, SW_GSUB, true), &outer, sizeof(outer));
	KUNIT_EXPECT_GT(test, work_record(test, 1, SW_MATCH, true)->total, inner);
	work_record(test, 2, SW_MATCH, true);
	work_run(test, "local t=setmetatable({}, {__index=function(t,x)"
		 "return '<'..string.match(x,'%d')..'>' end});"
		 "assert(string.gsub('a1b2','%d',t)=='a<1>b<2>')", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 3U);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 0, SW_GSUB, true), &outer, sizeof(outer));
	work_run(test, "assert(string.gsub('a1b2','%d',{['1']='one',['2']='two'})=='aonebtwo')", 0);
	KUNIT_EXPECT_EQ(test, work_record(test, 0, SW_GSUB, true)->count[SW_CALLBACK], 2ULL);
}

static void work_recovery_test(struct kunit *test)
{
	struct work_fixture *f = test->priv;
	struct lua_work_record saved;
	struct lua_work_record *r;

	work_run(test, "return string.match('abc123','(%a+)(%d+)')", 0);
	saved = *work_record(test, 0, SW_MATCH, true);
	work_run(test, "string.match('x','[')", LUA_ERRRUN);
	r = work_record(test, 0, SW_MATCH, false);
	KUNIT_EXPECT_EQ(test, r->count[SW_PATTERN], 2ULL);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "malformed pattern"));
	work_run(test, "string.gsub('a','a',function() error('replacement boom') end)", LUA_ERRRUN);
	r = work_record(test, 0, SW_GSUB, false);
	KUNIT_EXPECT_EQ(test, r->count[SW_CALLBACK], 1ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_OUTPUT], 1ULL);
	work_run(test, "assert(string.gsub('a','a',function(x)"
		 "assert(not pcall(string.match,'x','[')); return x end)=='a');"
		 "return string.match('abc123','(%a+)(%d+)')", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 3U);
	work_record(test, 0, SW_GSUB, true);
	work_record(test, 1, SW_MATCH, false);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 2, SW_MATCH, true), &saved, sizeof(saved));
	work_run(test, "local i=string.gmatch('x','['); assert(not pcall(i));"
		 "assert(not pcall(i)); return string.match('abc123','(%a+)(%d+)')", 0);
	KUNIT_ASSERT_EQ(test, f->observer.used, 3U);
	r = work_record(test, 0, SW_GMATCH, false);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 1, SW_GMATCH, false), r, sizeof(*r));
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 2, SW_MATCH, true), &saved, sizeof(saved));
	/* The existing capture limit still reports its own bounded error. */
	work_run(test, "return string.match('',string.rep('()',33))", LUA_ERRRUN);
	r = work_record(test, 0, SW_MATCH, false);
	KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], 33ULL);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "too many captures"));
	work_run(test, "return string.match('',string.rep('a*',65))", LUA_ERRRUN);
	r = work_record(test, 0, SW_MATCH, false);
	KUNIT_EXPECT_EQ(test, r->count[SW_ENTER], 66ULL);
	KUNIT_EXPECT_EQ(test, r->count[SW_DISPATCH], 65ULL);
	KUNIT_EXPECT_NOT_NULL(test, strstr(lua_tostring(f->L, -1), "pattern recursion limit exceeded"));
	work_run(test, "return string.match('abc123','(%a+)(%d+)')", 0);
	KUNIT_EXPECT_MEMEQ(test, work_record(test, 0, SW_MATCH, true), &saved, sizeof(saved));
}

static void work_integer_test(struct kunit *test)
{
	const u64 max = ~(u64)0;
	const u64 edge[] = { 0, 1, 7, 8, 9, (~(u64)0) - 1, ~(u64)0 };
	StrWorkBudget b;
	u64 total;
	unsigned int i, j;
	bool ok;

	/* Exhaust the small arithmetic domain without allocating large strings. */
	for (i = 0; i < 256; i++) {
		for (j = 0; j < 256; j++) {
			b = (StrWorkBudget){ .remaining = i };
			ok = strwork_debit(&b, j);
			if (ok != (j <= i) || b.remaining != (j <= i ? i - j : 0) ||
			    b.exceeded != (j > i)) {
				KUNIT_FAIL(test, "debit %u by %u", i, j);
				return;
			}
		}
	}
	for (i = 0; i < ARRAY_SIZE(edge); i++) {
		for (j = 0; j < ARRAY_SIZE(edge); j++) {
			b = (StrWorkBudget){ .remaining = edge[i] };
			KUNIT_EXPECT_EQ(test, strwork_debit(&b, edge[j]), edge[j] <= edge[i]);
			KUNIT_EXPECT_EQ(test, b.remaining, edge[j] <= edge[i] ? edge[i] - edge[j] : 0);
			KUNIT_EXPECT_EQ(test, b.exceeded, edge[j] > edge[i]);
		}
	}
	b = (StrWorkBudget){ .remaining = 8 };
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 7));
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 1));
	KUNIT_EXPECT_TRUE(test, strwork_debit(&b, 0));
	KUNIT_EXPECT_FALSE(test, b.exceeded);
	KUNIT_EXPECT_FALSE(test, strwork_debit(&b, 1));
	KUNIT_EXPECT_FALSE(test, strwork_debit(&b, 0));
	KUNIT_EXPECT_FALSE(test, strwork_debit(&b, max));
	KUNIT_EXPECT_EQ(test, b.remaining, 0ULL);
	KUNIT_EXPECT_TRUE(test, b.exceeded);
	total = max - 1;
	KUNIT_EXPECT_FALSE(test, strwork_accumulate(&total, 1));
	KUNIT_EXPECT_EQ(test, total, max);
	KUNIT_EXPECT_FALSE(test, strwork_accumulate(&total, 0));
	KUNIT_EXPECT_TRUE(test, strwork_accumulate(&total, 1));
	KUNIT_EXPECT_EQ(test, total, max);
	KUNIT_EXPECT_TRUE(test, strwork_accumulate(&total, max));
}

static int work_saturate_callback(lua_State *L)
{
	struct lua_work_observer *observer = lua_touserdata(L, lua_upvalueindex(1));

	/* Simulate a near-u64 observation without constructing huge strings. */
	observer->records[0].count[SW_OUTPUT] = ~(u64)0;
	observer->records[0].total = ~(u64)0;
	lua_pushvalue(L, 1);
	return 1;
}

static int work_saturate_run(lua_State *L)
{
	lua_getglobal(L, "string");
	lua_getfield(L, -1, "gsub");
	lua_pushliteral(L, "a");
	lua_pushliteral(L, "a");
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, work_saturate_callback, 1);
	lua_call(L, 3, 2);
	return 2;
}

static void work_observer_test(struct kunit *test)
{
	struct work_fixture *f = test->priv;
	struct lua_work_observer *other;
	struct lua_work_record saved;
	lua_State *L;
	const char *chunk = "return string.match('a','a')";
	int status, i;

	work_run(test, chunk, 0);
	saved = *work_record(test, 0, SW_MATCH, true);
	/* A second state has its own registry and externally owned record slots. */
	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, other);
	L = lua_newstate(work_alloc, NULL);
	KUNIT_ASSERT_NOT_NULL(test, L);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, work_close, L), 0);
	lua_pushcfunction(L, work_open);
	lua_pushlightuserdata(L, other);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 1, 0, 0), 0);
	KUNIT_ASSERT_EQ(test, luaL_loadbuffer(L, chunk, strlen(chunk), "other-state"), 0);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(L, -1), "a");
	KUNIT_EXPECT_EQ(test, other->used, 1U);
	KUNIT_EXPECT_MEMEQ(test, &other->records[0], &saved, sizeof(saved));
	KUNIT_EXPECT_EQ(test, f->observer.used, 1U);
	lua_work_observe(L, NULL);
	lua_settop(L, 0);
	KUNIT_ASSERT_EQ(test, luaL_loadbuffer(L, chunk, strlen(chunk), "detached"), 0);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 0, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, other->used, 1U);
	/* A full observer flags lost records without becoming a work limit. */
	memset(&f->observer, 0, sizeof(f->observer));
	for (i = 0; i < LUA_WORK_RECORDS; i++) {
		lua_settop(f->L, 0);
		KUNIT_ASSERT_EQ(test, luaL_loadbuffer(f->L, chunk, strlen(chunk), "observer-fill"), 0);
		KUNIT_ASSERT_EQ(test, lua_pcall(f->L, 0, 1, 0), 0);
		KUNIT_EXPECT_STREQ(test, lua_tostring(f->L, -1), "a");
	}
	KUNIT_EXPECT_FALSE(test, f->observer.full);
	KUNIT_ASSERT_EQ(test, f->observer.used, (unsigned int)LUA_WORK_RECORDS);
	lua_settop(f->L, 0);
	KUNIT_ASSERT_EQ(test, luaL_loadbuffer(f->L, chunk, strlen(chunk), "observer-full"), 0);
	status = lua_pcall(f->L, 0, 1, 0);
	KUNIT_ASSERT_EQ(test, status, 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(f->L, -1), "a");
	KUNIT_EXPECT_TRUE(test, f->observer.full);
	KUNIT_EXPECT_EQ(test, f->observer.used, (unsigned int)LUA_WORK_RECORDS);
	KUNIT_EXPECT_MEMEQ(test, &f->observer.records[0], &saved, sizeof(saved));
	lua_settop(f->L, 0);
	memset(&f->observer, 0, sizeof(f->observer));
	lua_pushcfunction(f->L, work_saturate_run);
	lua_pushlightuserdata(f->L, &f->observer);
	KUNIT_ASSERT_EQ(test, lua_pcall(f->L, 1, 2, 0), 0);
	KUNIT_EXPECT_STREQ(test, lua_tostring(f->L, -2), "a");
	KUNIT_ASSERT_EQ(test, f->observer.used, 1U);
	KUNIT_EXPECT_TRUE(test, f->observer.records[0].finished);
	KUNIT_EXPECT_TRUE(test, f->observer.records[0].overflow);
	KUNIT_EXPECT_EQ(test, f->observer.records[0].total, ~(u64)0);
	KUNIT_EXPECT_EQ(test, f->observer.records[0].count[SW_OUTPUT], ~(u64)0);
}

static struct kunit_case work_cases[] = {
	KUNIT_CASE(work_plain_test),
	KUNIT_CASE(work_binary_test),
	KUNIT_CASE(work_match_test),
	KUNIT_CASE(work_scan_test),
	KUNIT_CASE(work_capture_test),
	KUNIT_CASE(work_iterator_test),
	KUNIT_CASE(work_output_test),
	KUNIT_CASE(work_reentry_test),
	KUNIT_CASE(work_recovery_test),
	KUNIT_CASE(work_integer_test),
	KUNIT_CASE(work_observer_test),
	{}
};

static struct kunit_suite work_suite = {
	.name = "lua-string-work",
	.init = work_init,
	.test_cases = work_cases,
};

kunit_test_suite(work_suite);

MODULE_DESCRIPTION("KUnit observations of Lua string work units");
MODULE_LICENSE("GPL");
