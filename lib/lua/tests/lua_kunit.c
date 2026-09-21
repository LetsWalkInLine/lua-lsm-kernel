// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/lauxlib.h>
#include <linux/lua.h>
#include <linux/lualib.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>


/* The check chunk receives the exact result tuple, including trailing nils. */
struct lua_kunit_vector {
	const char *id;
	const char *chunk;
	const char *check;
	int (*invoke)(lua_State *L);
};

#define LUA_POSITION_CAPTURES_8	"()()()()()()()()"
#define LUA_POSITION_CAPTURES_32	(LUA_POSITION_CAPTURES_8 \
				 LUA_POSITION_CAPTURES_8 \
				 LUA_POSITION_CAPTURES_8 \
				 LUA_POSITION_CAPTURES_8)

static void *lua_kunit_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud;
	(void)osize;

	if (!nsize) {
		kfree(ptr);
		return NULL;
	}

	return krealloc(ptr, nsize, GFP_KERNEL);
}

static void lua_kunit_close(void *data)
{
	lua_close(data);
}

static lua_State *lua_kunit_new_state(struct kunit *test)
{
	lua_State *L;
	int err;

	L = lua_newstate(lua_kunit_alloc, NULL);
	KUNIT_ASSERT_NOT_NULL(test, L);

	err = kunit_add_action_or_reset(test, lua_kunit_close, L);
	KUNIT_ASSERT_EQ(test, err, 0);

	return L;
}

static void lua_kunit_open_library(struct kunit *test, lua_State *L,
				   int (*open_library)(lua_State *L),
				   const char *name)
{
	const char *error;
	int status;

	lua_pushcfunction(L, open_library);
	lua_pushstring(L, name);
	status = lua_pcall(L, 1, 0, 0);
	error = status ? lua_tostring(L, -1) : NULL;
	KUNIT_ASSERT_EQ_MSG(test, status, 0, "cannot open Lua library '%s': %s",
			    name, error ? error : "non-string Lua error");
}

static void lua_kunit_open_string(struct kunit *test, lua_State *L)
{
	lua_kunit_open_library(test, L, luaopen_string, LUA_STRLIBNAME);
}

static lua_State *lua_kunit_new_behavior_state(struct kunit *test)
{
	lua_State *L = lua_kunit_new_state(test);

	/* pcall and error are used to observe Lua-visible semantics. */
	lua_kunit_open_library(test, L, luaopen_base, "");
	lua_kunit_open_string(test, L);

	return L;
}

static void lua_kunit_push_string_function(lua_State *L, const char *name)
{
	lua_getglobal(L, LUA_STRLIBNAME);
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
}

static int lua_kunit_invoke_k02(lua_State *L)
{
	bool all_one = true;
	int result_count;
	int status;
	int i;

	lua_kunit_push_string_function(L, "match");
	lua_pushliteral(L, "");
	lua_pushstring(L, LUA_POSITION_CAPTURES_32);
	status = lua_pcall(L, 2, LUA_MULTRET, 0);
	if (status)
		return status;

	result_count = lua_gettop(L);
	for (i = 1; i <= result_count; i++)
		if (lua_type(L, i) != LUA_TNUMBER || lua_tointeger(L, i) != 1)
			all_one = false;

	lua_settop(L, 0);
	lua_pushinteger(L, result_count);
	lua_pushboolean(L, all_one);
	return 0;
}

static int lua_kunit_invoke_i01(lua_State *L)
{
	int first_count;
	int first_ref;
	int second_base;
	int second_count;
	int status;

	lua_kunit_push_string_function(L, "gmatch");
	lua_pushliteral(L, "aaaaaaaaaaaaaaaab");
	lua_pushliteral(L, "b");
	status = lua_pcall(L, 2, 1, 0);
	if (status)
		return status;

	lua_pushvalue(L, 1);
	status = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (status)
		return status;
	first_count = lua_gettop(L) - 1;

	if (first_count == 1)
		lua_pushvalue(L, 2);
	else
		lua_pushnil(L);
	first_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	second_base = lua_gettop(L);
	lua_pushvalue(L, 1);
	status = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (status) {
		luaL_unref(L, LUA_REGISTRYINDEX, first_ref);
		return status;
	}
	second_count = lua_gettop(L) - second_base;

	lua_settop(L, 0);
	lua_pushinteger(L, first_count);
	lua_rawgeti(L, LUA_REGISTRYINDEX, first_ref);
	luaL_unref(L, LUA_REGISTRYINDEX, first_ref);
	lua_pushinteger(L, second_count);
	return 0;
}

static bool lua_kunit_run_vector(struct kunit *test, lua_State *L,
				 const struct lua_kunit_vector *vector)
{
	int status, count;

	lua_settop(L, 0);
	if (vector->invoke) {
		status = vector->invoke(L);
	} else {
		status = luaL_loadbuffer(L, vector->chunk, strlen(vector->chunk),
					vector->id);
		if (!status)
			status = lua_pcall(L, 0, LUA_MULTRET, 0);
	}
	if (!status) {
		count = lua_gettop(L);
		lua_pushliteral(L, "local n = select('#', ...); local a,b,c,d = ...; ");
		lua_pushstring(L, vector->check);
		lua_concat(L, 2);
		status = luaL_loadstring(L, lua_tostring(L, -1));
		lua_remove(L, -2);
		if (!status) {
			lua_insert(L, 1);
			status = lua_pcall(L, count, 0, 0);
		}
	}
	if (status)
		KUNIT_FAIL(test, "%s: %s", vector->id, lua_tostring(L, -1));
	return !status;
}

static void lua_kunit_run_vectors(struct kunit *test, lua_State *L,
				  const struct lua_kunit_vector *vectors,
				  size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		KUNIT_EXPECT_TRUE_MSG(test,
				      lua_kunit_run_vector(test, L, &vectors[i]),
				      "%s failed", vectors[i].id);
}

static const struct lua_kunit_vector basic_vectors[] = {
	{ "B01", "return string.match('abc123', '(%a+)(%d+)')\n",
	  "assert(n == 2 and a == \"abc\" and b == \"123\")\n" },
	{ "B02", "return string.find('--abc123--', '(%a+)(%d+)',\n1, false)\n",
	  "assert(n == 4 and a == 3 and b == 8 and c == \"abc\" and d == \"123\")\n" },
	{ "B03", "return string.find('', '', 1, true)\n",
	  "assert(n == 2 and a == 1 and b == 0)\n" },
	{ "B04", "return string.match('', '^$')\n",
	  "assert(n == 1 and a == \"\")\n" },
	{ "B05", "return string.match('abc', '^b')\n",
	  "assert(n == 1 and a == nil)\n" },
};

static const struct lua_kunit_vector plain_find_vectors[] = {
	{ "P01", "return string.find('xx/secret', '/secret', 1, true)\n",
	  "assert(n == 2 and a == 3 and b == 9)\n" },
	{ "P02", "return string.find(string.rep('a', 16), 'aaaaab',\n1, true)\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "P03", "local a = string.rep('a', 32); return string.find(\na .. 'c', a .. 'b', "
	  "1, true)\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "P04", "return string.find('x', string.rep('a', 64),\n1, false)\n",
	  "assert(n == 1 and a == nil)\n" },
};

static const struct lua_kunit_vector search_path_vectors[] = {
	{ "S01", "return string.match(string.rep('a', 16), 'aaab')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "R01", "local a = string.rep('a', 8); return string.match(\na .. 'b', '^' .. "
	  "string.rep('a?', 8) .. 'b')\n",
	  "assert(n == 1 and a == \"aaaaaaaab\")\n" },
	{ "R02", "local a = string.rep('a', 8); return string.match(\na, '^' .. "
	  "string.rep('a?', 8) .. 'b')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "R03", "return string.match(string.rep('a', 16),\n'^a*a*a*b')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "R04", "return string.match(string.rep('a', 16),\n'^a-a-a-b')\n",
	  "assert(n == 1 and a == nil)\n" },
};

static const struct lua_kunit_vector class_balance_vectors[] = {
	{ "C01", "return string.match(string.rep('x', 16), '[' ..\nstring.rep('a', 64) .. "
	  "']z')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "C02", "return string.match('b', '[%%a-cx]')\n",
	  "assert(n == 1 and a == \"b\")\n" },
	{ "F01", "return string.match(string.rep('x', 16), '%f[' ..\nstring.rep('a', 64) "
	  ".. ']z')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "F02", "return string.match('foo bar', '%f[%a]bar')\n",
	  "assert(n == 1 and a == \"bar\")\n" },
	{ "L01", "local s = '(' .. string.rep('(', 16) ..\nstring.rep(')', 16) .. ')'; "
	  "return string.match(s, '%b()')\n",
	  "assert(n == 1 and a == \"((((((((((((((((()))))))))))))))))\")\n" },
	{ "L02", "local s = string.rep('(', 17) ..\nstring.rep(')', 16); return "
	  "string.match(s, '^%b()$')\n",
	  "assert(n == 1 and a == nil)\n" },
};

static const struct lua_kunit_vector capture_vectors[] = {
	{ "K01", "return string.match(string.rep('a', 32),\n'^(a+)%1b')\n",
	  "assert(n == 1 and a == nil)\n" },
	{ "K02", NULL,
	  "assert(n == 2 and a == 32 and b == true)\n", lua_kunit_invoke_k02 },
	{ "K03", "local ok, err = pcall(string.match, '',\nstring.rep('()', 33)); return "
	  "ok, err,\nstring.match('abc123', '(%a+)(%d+)')\n",
	  "assert(n == 4 and a == false and type(b) == \"string\" and string.find(b,"
	  " \"too many captures\", 1, true) and c == \"abc\" and d == \"123\")\n" },
};

static const struct lua_kunit_vector iterator_vectors[] = {
	{ "I01", NULL,
	  "assert(n == 3 and a == 1 and b == \"b\" and c == 0)\n", lua_kunit_invoke_i01 },
	{ "I02", "local p = {}; for v in string.gmatch('ab', '()') do\np[#p + 1] = v end; "
	  "return #p, p[1], p[2], p[3]\n",
	  "assert(n == 4 and a == 3 and b == 1 and c == 2 and d == 3)\n" },
	{ "I03", "local c = {}; for v in string.gfind('ab', '.') do\nc[#c + 1] = v end; "
	  "return #c, c[1], c[2]\n",
	  "assert(n == 3 and a == 2 and b == \"a\" and c == \"b\")\n" },
};

static const struct lua_kunit_vector gsub_vectors[] = {
	{ "U01", "return string.gsub(string.rep('a', 16), 'b', 'x')\n",
	  "assert(n == 2 and a == \"aaaaaaaaaaaaaaaa\" and b == 0)\n" },
	{ "U02", "return string.gsub('aaaa', 'a', '%0%0')\n",
	  "assert(n == 2 and a == \"aaaaaaaa\" and b == 4)\n" },
	{ "U03", "return string.gsub('aaaa', '(a)', '%1%1')\n",
	  "assert(n == 2 and a == \"aaaaaaaa\" and b == 4)\n" },
	{ "U04", "return string.gsub('a1b2', '%d',\n{['1'] = 'one', ['2'] = 'two'})\n",
	  "assert(n == 2 and a == \"aonebtwo\" and b == 2)\n" },
	{ "U05", "return string.gsub('a1b2', '%d', function(x)\nreturn '[' .. x .. ']' "
	  "end)\n",
	  "assert(n == 2 and a == \"a[1]b[2]\" and b == 2)\n" },
	{ "U06", "return string.gsub('aaaa', 'a', 'x', 2)\n",
	  "assert(n == 2 and a == \"xxaa\" and b == 2)\n" },
	{ "U07", "return string.gsub('ab', '()', '-')\n",
	  "assert(n == 2 and a == \"-a-b-\" and b == 3)\n" },
	{ "U08", "local ok, err = pcall(string.gsub, 'a', 'a',\nfunction() "
	  "error('replacement boom') end); return ok, err,\nstring.match('abc123', "
	  "'(%a+)(%d+)')\n",
	  "assert(n == 4 and a == false and type(b) == \"string\" and string.find(b,"
	  " \"replacement boom\", 1, true) and c == \"abc\" and d == \"123\")\n" },
	{ "U09", "return string.gsub('a1b2', '%d', function(x)\nreturn '<' .. "
	  "string.match(x, '%d') .. '>' end)\n",
	  "assert(n == 2 and a == \"a<1>b<2>\" and b == 2)\n" },
};

static const struct lua_kunit_vector malformed_vectors[] = {
	{ "E01", "local ok, err = pcall(string.match, 'x', '['); return ok, err,\n"
	  "string.match('abc123', '(%a+)(%d+)')\n",
	  "assert(n == 4 and a == false and type(b) == \"string\" and string.find(b,"
	  " \"malformed pattern (missing ']')\", 1, true) and c == \"abc\" and d == "
	  "\"123\")\n" },
};

static void lua_state_lifecycle_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_state(test);

	KUNIT_EXPECT_EQ(test, lua_gettop(L), 0);
	lua_kunit_open_string(test, L);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 0);

	lua_getglobal(L, LUA_STRLIBNAME);
	KUNIT_ASSERT_TRUE(test, lua_istable(L, -1));
	lua_getfield(L, -1, "match");
	KUNIT_EXPECT_TRUE(test, lua_isfunction(L, -1));
}

static void lua_string_basic_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, basic_vectors, ARRAY_SIZE(basic_vectors));
}

static void lua_string_plain_find_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, plain_find_vectors,
			      ARRAY_SIZE(plain_find_vectors));
}

static void lua_string_search_paths_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, search_path_vectors,
			      ARRAY_SIZE(search_path_vectors));
}

static void lua_string_class_balance_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, class_balance_vectors,
			      ARRAY_SIZE(class_balance_vectors));
}

static void lua_string_capture_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, capture_vectors,
			      ARRAY_SIZE(capture_vectors));
}

static void lua_string_iterator_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, iterator_vectors,
			      ARRAY_SIZE(iterator_vectors));
}

static void lua_string_gsub_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, gsub_vectors,
			      ARRAY_SIZE(gsub_vectors));
}

static void lua_string_malformed_pattern_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, malformed_vectors,
			      ARRAY_SIZE(malformed_vectors));
}

static struct kunit_case lua_string_test_cases[] = {
	KUNIT_CASE(lua_state_lifecycle_test),
	KUNIT_CASE(lua_string_basic_test),
	KUNIT_CASE(lua_string_plain_find_test),
	KUNIT_CASE(lua_string_search_paths_test),
	KUNIT_CASE(lua_string_class_balance_test),
	KUNIT_CASE(lua_string_capture_test),
	KUNIT_CASE(lua_string_iterator_test),
	KUNIT_CASE(lua_string_gsub_test),
	KUNIT_CASE(lua_string_malformed_pattern_test),
	{}
};

static struct kunit_suite lua_string_test_suite = {
	.name = "lua-string",
	.test_cases = lua_string_test_cases,
};

kunit_test_suite(lua_string_test_suite);

MODULE_DESCRIPTION("KUnit tests for the in-kernel Lua string library");
MODULE_LICENSE("GPL");
