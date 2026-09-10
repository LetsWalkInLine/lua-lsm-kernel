// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/lauxlib.h>
#include <linux/lua.h>
#include <linux/lualib.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lua_depth_test.h"
#include "lua_work_test.h"

enum lua_kunit_result_kind {
	LUA_KUNIT_STRING,
	LUA_KUNIT_STRING_CONTAINS,
	LUA_KUNIT_INTEGER,
	LUA_KUNIT_BOOLEAN,
	LUA_KUNIT_NIL,
};

struct lua_kunit_expected_result {
	enum lua_kunit_result_kind kind;
	union {
		const char *string;
		long long integer;
		bool boolean;
	};
};

#define LUA_EXPECT_STRING(value_) \
	{ .kind = LUA_KUNIT_STRING, .string = (value_) }
#define LUA_EXPECT_STRING_CONTAINS(value_) \
	{ .kind = LUA_KUNIT_STRING_CONTAINS, .string = (value_) }
#define LUA_EXPECT_INTEGER(value_) \
	{ .kind = LUA_KUNIT_INTEGER, .integer = (value_) }
#define LUA_EXPECT_BOOLEAN(value_) \
	{ .kind = LUA_KUNIT_BOOLEAN, .boolean = (value_) }
#define LUA_EXPECT_NIL() \
	{ .kind = LUA_KUNIT_NIL }

struct lua_kunit_vector {
	const char *id;
	const char *chunk;
	int (*invoke)(lua_State *L);
	struct lua_kunit_expected_result expected[4];
	size_t expected_count;
};

#define LUA_SOURCE_A_16	"aaaaaaaaaaaaaaaa"

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

static bool lua_kunit_check_result(struct kunit *test, lua_State *L,
				   const struct lua_kunit_vector *vector,
				   size_t result)
{
	const struct lua_kunit_expected_result *expected;
	const char *actual_string;
	int index = result + 1;
	int actual_type;

	expected = &vector->expected[result];
	actual_type = lua_type(L, index);

	switch (expected->kind) {
	case LUA_KUNIT_STRING:
	case LUA_KUNIT_STRING_CONTAINS:
		if (actual_type != LUA_TSTRING) {
			KUNIT_FAIL(test, "%s result %zu: expected string, got %s",
				   vector->id, result + 1,
				   lua_typename(L, actual_type));
			return false;
		}

		actual_string = lua_tostring(L, index);
		if (expected->kind == LUA_KUNIT_STRING &&
		    strcmp(actual_string, expected->string)) {
			KUNIT_FAIL(test, "%s result %zu: got '%s', expected '%s'",
				   vector->id, result + 1, actual_string,
				   expected->string);
			return false;
		}
		if (expected->kind == LUA_KUNIT_STRING_CONTAINS &&
		    !strstr(actual_string, expected->string)) {
			KUNIT_FAIL(test,
				   "%s result %zu: '%s' does not contain '%s'",
				   vector->id, result + 1, actual_string,
				   expected->string);
			return false;
		}
		return true;
	case LUA_KUNIT_INTEGER:
		if (actual_type != LUA_TNUMBER ||
		    (long long)lua_tointeger(L, index) != expected->integer) {
			KUNIT_FAIL(test, "%s result %zu: got %lld, expected %lld",
				   vector->id, result + 1,
				   (long long)lua_tointeger(L, index),
				   (long long)expected->integer);
			return false;
		}
		return true;
	case LUA_KUNIT_BOOLEAN:
		if (actual_type != LUA_TBOOLEAN ||
		    lua_toboolean(L, index) != expected->boolean) {
			KUNIT_FAIL(test, "%s result %zu: got %s, expected %s",
				   vector->id, result + 1,
				   lua_toboolean(L, index) ? "true" : "false",
				   expected->boolean ? "true" : "false");
			return false;
		}
		return true;
	case LUA_KUNIT_NIL:
		if (actual_type != LUA_TNIL) {
			KUNIT_FAIL(test, "%s result %zu: expected nil, got %s",
				   vector->id, result + 1,
				   lua_typename(L, actual_type));
			return false;
		}
		return true;
	}

	KUNIT_FAIL(test, "%s result %zu has an unknown expectation kind",
		   vector->id, result + 1);
	return false;
}

static bool lua_kunit_run_vector(struct kunit *test, lua_State *L,
				 const struct lua_kunit_vector *vector)
{
	const char *error;
	size_t i;
	int status;

	lua_settop(L, 0);
	if (vector->invoke) {
		status = vector->invoke(L);
	} else {
		status = luaL_loadbuffer(L, vector->chunk, strlen(vector->chunk),
					 vector->id);
		if (status) {
			error = lua_tostring(L, -1);
			KUNIT_FAIL(test, "%s load failed: %s", vector->id,
				   error ? error : "non-string Lua error");
			return false;
		}

		status = lua_pcall(L, 0, LUA_MULTRET, 0);
	}
	if (status) {
		error = lua_tostring(L, -1);
		KUNIT_FAIL(test, "%s execution failed: %s", vector->id,
			   error ? error : "non-string Lua error");
		return false;
	}

	if ((size_t)lua_gettop(L) != vector->expected_count) {
		KUNIT_FAIL(test, "%s returned %d values, expected %zu",
			   vector->id, lua_gettop(L), vector->expected_count);
		return false;
	}

	for (i = 0; i < vector->expected_count; i++)
		if (!lua_kunit_check_result(test, L, vector, i))
			return false;

	return true;
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
	{
		.id = "B01",
		.chunk = "return string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 2,
	},
	{
		.id = "B02",
		.chunk = "return string.find('--abc123--', '(%a+)(%d+)',\n"
			 "1, false)",
		.expected = {
			LUA_EXPECT_INTEGER(3),
			LUA_EXPECT_INTEGER(8),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "B03",
		.chunk = "return string.find('', '', 1, true)",
		.expected = {
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_INTEGER(0),
		},
		.expected_count = 2,
	},
	{
		.id = "B04",
		.chunk = "return string.match('', '^$')",
		.expected = { LUA_EXPECT_STRING("") },
		.expected_count = 1,
	},
	{
		.id = "B05",
		.chunk = "return string.match('abc', '^b')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector plain_find_vectors[] = {
	{
		.id = "P01",
		.chunk = "return string.find('xx/secret', '/secret', 1, true)",
		.expected = {
			LUA_EXPECT_INTEGER(3),
			LUA_EXPECT_INTEGER(9),
		},
		.expected_count = 2,
	},
	{
		.id = "P02",
		.chunk = "return string.find(string.rep('a', 16), 'aaaaab',\n"
			 "1, true)",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "P03",
		.chunk = "local a = string.rep('a', 32); return string.find(\n"
			 "a .. 'c', a .. 'b', 1, true)",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "P04",
		.chunk = "return string.find('x', string.rep('a', 64),\n"
			 "1, false)",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector search_path_vectors[] = {
	{
		.id = "S01",
		.chunk = "return string.match(string.rep('a', 16), 'aaab')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "R01",
		.chunk = "local a = string.rep('a', 8); return string.match(\n"
			 "a .. 'b', '^' .. string.rep('a?', 8) .. 'b')",
		.expected = { LUA_EXPECT_STRING("aaaaaaaab") },
		.expected_count = 1,
	},
	{
		.id = "R02",
		.chunk = "local a = string.rep('a', 8); return string.match(\n"
			 "a, '^' .. string.rep('a?', 8) .. 'b')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "R03",
		.chunk = "return string.match(string.rep('a', 16),\n"
			 "'^a*a*a*b')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "R04",
		.chunk = "return string.match(string.rep('a', 16),\n"
			 "'^a-a-a-b')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector class_balance_vectors[] = {
	{
		.id = "C01",
		.chunk = "return string.match(string.rep('x', 16), '[' ..\n"
			 "string.rep('a', 64) .. ']z')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "C02",
		.chunk = "return string.match('b', '[%%a-cx]')",
		.expected = { LUA_EXPECT_STRING("b") },
		.expected_count = 1,
	},
	{
		.id = "F01",
		.chunk = "return string.match(string.rep('x', 16), '%f[' ..\n"
			 "string.rep('a', 64) .. ']z')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "F02",
		.chunk = "return string.match('foo bar', '%f[%a]bar')",
		.expected = { LUA_EXPECT_STRING("bar") },
		.expected_count = 1,
	},
	{
		.id = "L01",
		.chunk = "local s = '(' .. string.rep('(', 16) ..\n"
			 "string.rep(')', 16) .. ')'; return string.match(s, '%b()')",
		.expected = {
			LUA_EXPECT_STRING("((((((((((((((((()))))))))))))))))"),
		},
		.expected_count = 1,
	},
	{
		.id = "L02",
		.chunk = "local s = string.rep('(', 17) ..\n"
			 "string.rep(')', 16); return string.match(s, '^%b()$')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector capture_vectors[] = {
	{
		.id = "K01",
		.chunk = "return string.match(string.rep('a', 32),\n"
			 "'^(a+)%1b')",
		.expected = { LUA_EXPECT_NIL() },
		.expected_count = 1,
	},
	{
		.id = "K02",
		.invoke = lua_kunit_invoke_k02,
		.expected = {
			LUA_EXPECT_INTEGER(32),
			LUA_EXPECT_BOOLEAN(true),
		},
		.expected_count = 2,
	},
	{
		.id = "K03",
		.chunk = "local ok, err = pcall(string.match, '',\n"
			 "string.rep('()', 33)); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("too many captures"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
};

static const struct lua_kunit_vector iterator_vectors[] = {
	{
		.id = "I01",
		.invoke = lua_kunit_invoke_i01,
		.expected = {
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_STRING("b"),
			LUA_EXPECT_INTEGER(0),
		},
		.expected_count = 3,
	},
	{
		.id = "I02",
		.chunk = "local p = {}; for v in string.gmatch('ab', '()') do\n"
			 "p[#p + 1] = v end; return #p, p[1], p[2], p[3]",
		.expected = {
			LUA_EXPECT_INTEGER(3),
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_INTEGER(2),
			LUA_EXPECT_INTEGER(3),
		},
		.expected_count = 4,
	},
	{
		.id = "I03",
		.chunk = "local c = {}; for v in string.gfind('ab', '.') do\n"
			 "c[#c + 1] = v end; return #c, c[1], c[2]",
		.expected = {
			LUA_EXPECT_INTEGER(2),
			LUA_EXPECT_STRING("a"),
			LUA_EXPECT_STRING("b"),
		},
		.expected_count = 3,
	},
};

static const struct lua_kunit_vector gsub_vectors[] = {
	{
		.id = "U01",
		.chunk = "return string.gsub(string.rep('a', 16), 'b', 'x')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaaaaaaaaaaa"),
			LUA_EXPECT_INTEGER(0),
		},
		.expected_count = 2,
	},
	{
		.id = "U02",
		.chunk = "return string.gsub('aaaa', 'a', '%0%0')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaaa"),
			LUA_EXPECT_INTEGER(4),
		},
		.expected_count = 2,
	},
	{
		.id = "U03",
		.chunk = "return string.gsub('aaaa', '(a)', '%1%1')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaaa"),
			LUA_EXPECT_INTEGER(4),
		},
		.expected_count = 2,
	},
	{
		.id = "U04",
		.chunk = "return string.gsub('a1b2', '%d',\n"
			 "{['1'] = 'one', ['2'] = 'two'})",
		.expected = {
			LUA_EXPECT_STRING("aonebtwo"),
			LUA_EXPECT_INTEGER(2),
		},
		.expected_count = 2,
	},
	{
		.id = "U05",
		.chunk = "return string.gsub('a1b2', '%d', function(x)\n"
			 "return '[' .. x .. ']' end)",
		.expected = {
			LUA_EXPECT_STRING("a[1]b[2]"),
			LUA_EXPECT_INTEGER(2),
		},
		.expected_count = 2,
	},
	{
		.id = "U06",
		.chunk = "return string.gsub('aaaa', 'a', 'x', 2)",
		.expected = {
			LUA_EXPECT_STRING("xxaa"),
			LUA_EXPECT_INTEGER(2),
		},
		.expected_count = 2,
	},
	{
		.id = "U07",
		.chunk = "return string.gsub('ab', '()', '-')",
		.expected = {
			LUA_EXPECT_STRING("-a-b-"),
			LUA_EXPECT_INTEGER(3),
		},
		.expected_count = 2,
	},
	{
		.id = "U08",
		.chunk = "local ok, err = pcall(string.gsub, 'a', 'a',\n"
			 "function() error('replacement boom') end);"
			 " return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("replacement boom"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "U09",
		.chunk = "return string.gsub('a1b2', '%d', function(x)\n"
			 "return '<' .. string.match(x, '%d') .. '>' end)",
		.expected = {
			LUA_EXPECT_STRING("a<1>b<2>"),
			LUA_EXPECT_INTEGER(2),
		},
		.expected_count = 2,
	},
};

static const struct lua_kunit_vector malformed_vectors[] = {
	{
		.id = "E01",
		.chunk = "local ok, err = pcall(string.match, 'x', '[');"
			 " return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("malformed pattern (missing ']')"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
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

static int lua_kunit_attach_work(lua_State *L)
{
	lua_work_observe(L, lua_touserdata(L, 1));
	return 0;
}

static void lua_kunit_log_work(struct kunit *test, const char *id,
			       unsigned int operation, const struct lua_work_record *r)
{
	/* Development observations; order is StrWorkKind, not a public ABI. */
	char counts[SW_KINDS * 21];
	size_t used = 0;
	int k;

	for (k = 0; k < SW_KINDS; k++)
		used += scnprintf(counts + used, sizeof(counts) - used,
				  "%s%llu", k ? "," : "", r->count[k]);
	kunit_info(test, "work-count %s op=%u api=%d total=%llu finished=%d counts=%s\n",
		   id, operation, r->api, r->total, r->finished, counts);
}

static void lua_string_work_behavior_test(struct kunit *test)
{
	static const struct {
		const struct lua_kunit_vector *vectors;
		size_t count;
	} groups[] = {
		{ basic_vectors, ARRAY_SIZE(basic_vectors) },
		{ plain_find_vectors, ARRAY_SIZE(plain_find_vectors) },
		{ search_path_vectors, ARRAY_SIZE(search_path_vectors) },
		{ class_balance_vectors, ARRAY_SIZE(class_balance_vectors) },
		{ capture_vectors, ARRAY_SIZE(capture_vectors) },
		{ iterator_vectors, ARRAY_SIZE(iterator_vectors) },
		{ gsub_vectors, ARRAY_SIZE(gsub_vectors) },
		{ malformed_vectors, ARRAY_SIZE(malformed_vectors) },
	};
	struct lua_work_observer *observer;
	lua_State *L;
	size_t g, v;
	unsigned int i;

	observer = kunit_kzalloc(test, sizeof(*observer), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, observer);
	L = lua_kunit_new_state(test);
	lua_kunit_open_library(test, L, luaopen_base, "");
	lua_kunit_open_library(test, L, luaopen_string_work, LUA_STRLIBNAME);
	lua_pushcfunction(L, lua_kunit_attach_work);
	lua_pushlightuserdata(L, observer);
	KUNIT_ASSERT_EQ(test, lua_pcall(L, 1, 0, 0), 0);
	for (g = 0; g < ARRAY_SIZE(groups); g++) {
		for (v = 0; v < groups[g].count; v++) {
			memset(observer, 0, sizeof(*observer));
			KUNIT_EXPECT_TRUE_MSG(test,
				lua_kunit_run_vector(test, L, &groups[g].vectors[v]),
				"observed %s", groups[g].vectors[v].id);
			KUNIT_EXPECT_FALSE(test, observer->full);
			KUNIT_EXPECT_GT(test, observer->used, 0U);
			for (i = 0; i < observer->used; i++) {
				KUNIT_EXPECT_FALSE(test, observer->records[i].overflow);
				lua_kunit_log_work(test, groups[g].vectors[v].id,
						   i, &observer->records[i]);
			}
		}
	}
	lua_work_observe(L, NULL);
}

static int lua_kunit_invoke_depth_retry(lua_State *L)
{
	int status;
	int i;

	lua_kunit_push_string_function(L, "gmatch");
	lua_pushliteral(L, "aaaaaaaab");
	lua_pushliteral(L, "a?a?a?a?a?a?a?a?b");
	status = lua_pcall(L, 2, 1, 0);
	if (status)
		return status;
	for (i = 0; i < 2; i++) {
		lua_pushvalue(L, 1);
		status = lua_pcall(L, 0, 1, 0);
		if (status != LUA_ERRRUN)
			return status ? status : LUA_ERRRUN;
	}
	lua_remove(L, 1);
	lua_kunit_push_string_function(L, "match");
	lua_pushliteral(L, "ab");
	lua_pushliteral(L, "a?b");
	return lua_pcall(L, 2, 1, 0);
}

static int lua_kunit_invoke_depth_iteration(lua_State *L)
{
	bool all_match = true;
	int status;
	int i;

	lua_kunit_push_string_function(L, "gmatch");
	lua_pushliteral(L, "abababababababababababab");
	lua_pushliteral(L, "a?b");
	status = lua_pcall(L, 2, 1, 0);
	if (status)
		return status;
	for (i = 0; i < 13; i++) {
		lua_settop(L, 1);
		lua_pushvalue(L, 1);
		status = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (status)
			return status;
		if (i == 12) {
			all_match &= lua_gettop(L) == 1;
		} else if (lua_gettop(L) != 2 || lua_type(L, 2) != LUA_TSTRING ||
			   strcmp(lua_tostring(L, 2), "ab")) {
			all_match = false;
		}
	}
	lua_settop(L, 0);
	lua_pushboolean(L, all_match);
	return 0;
}

static const struct lua_kunit_vector depth_optional_vectors[] = {
	{
		.id = "DO6",
		.chunk = "return string.match(string.rep('a', 6) .. 'b', '^' ..\n"
			 "string.rep('a?', 6) .. 'b')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaab"),
		},
		.expected_count = 1,
	},
	{
		.id = "DO7",
		.chunk = "return string.match(string.rep('a', 7) .. 'b', '^' ..\n"
			 "string.rep('a?', 7) .. 'b')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaab"),
		},
		.expected_count = 1,
	},
	{
		.id = "DO8",
		.chunk = "local ok, err = pcall(string.match, 'aaaaaaaab', '^' ..\n"
			 "string.rep('a?', 8) .. 'b'); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DO-fail",
		.chunk = "return string.match('aaaaaaa', '^' .. string.rep('a?', 7) ..\n"
			 "'b')",
		.expected = {
			LUA_EXPECT_NIL(),
		},
		.expected_count = 1,
	},
	{
		.id = "DO-fallback",
		.chunk = "return string.match('a', '^a?a$')",
		.expected = {
			LUA_EXPECT_STRING("a"),
		},
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector depth_capture_vectors[] = {
	{
		.id = "DC-position7",
		.chunk = "return select('#', string.match('', string.rep('()', 7))),\n"
			 "string.match('', '()')",
		.expected = {
			LUA_EXPECT_INTEGER(7),
			LUA_EXPECT_INTEGER(1),
		},
		.expected_count = 2,
	},
	{
		.id = "DC-position8",
		.chunk = "local ok, err = pcall(string.match, '', string.rep('()', 8));\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DC-normal3",
		.chunk = "return string.match('aaa', '(a)(a)(a)')",
		.expected = {
			LUA_EXPECT_STRING("a"),
			LUA_EXPECT_STRING("a"),
			LUA_EXPECT_STRING("a"),
		},
		.expected_count = 3,
	},
	{
		.id = "DC-normal4",
		.chunk = "local ok, err = pcall(string.match, 'aaaa', '(a)(a)(a)(a)');\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DC-nested3",
		.chunk = "return string.match('a', '(((a)))')",
		.expected = {
			LUA_EXPECT_STRING("a"),
			LUA_EXPECT_STRING("a"),
			LUA_EXPECT_STRING("a"),
		},
		.expected_count = 3,
	},
	{
		.id = "DC-nested4",
		.chunk = "local ok, err = pcall(string.match, 'a', '((((a))))'); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
};

static const struct lua_kunit_vector depth_expand_vectors[] = {
	{
		.id = "DX-greedy7",
		.chunk = "return string.match('', '^' .. string.rep('a*', 7) .. '$')",
		.expected = {
			LUA_EXPECT_STRING(""),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-greedy8",
		.chunk = "local ok, err = pcall(string.match, '', '^' .. string.rep('a*',\n"
			 "8) .. '$'); return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DX-greedy-fail",
		.chunk = "return string.match('aaaa', '^a*a*b$')",
		.expected = {
			LUA_EXPECT_NIL(),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-greedy-backtrack",
		.chunk = "return string.match('aaaa', '^a*a$')",
		.expected = {
			LUA_EXPECT_STRING("aaaa"),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-minimal7",
		.chunk = "return string.match('', '^' .. string.rep('a-', 7) .. '$')",
		.expected = {
			LUA_EXPECT_STRING(""),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-minimal8",
		.chunk = "local ok, err = pcall(string.match, '', '^' .. string.rep('a-',\n"
			 "8) .. '$'); return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DX-minimal-fail",
		.chunk = "return string.match('aaaa', '^a-a-b$')",
		.expected = {
			LUA_EXPECT_NIL(),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-minimal-backtrack",
		.chunk = "return string.match('aaaa', '^a-a$')",
		.expected = {
			LUA_EXPECT_STRING("aaaa"),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-plus7",
		.chunk = "return string.match('aaaaaaa', '^' .. string.rep('a+', 7) ..\n"
			 "'$')",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaa"),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-plus8",
		.chunk = "local ok, err = pcall(string.match, 'aaaaaaaa', '^' ..\n"
			 "string.rep('a+', 8) .. '$'); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DX-plus-fail",
		.chunk = "return string.match('aaaa', '^a+a+b$')",
		.expected = {
			LUA_EXPECT_NIL(),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-plus-backtrack",
		.chunk = "return string.match('aaaa', '^a+a$')",
		.expected = {
			LUA_EXPECT_STRING("aaaa"),
		},
		.expected_count = 1,
	},
	{
		.id = "DX-plus-empty",
		.chunk = "return string.match('', 'a+')",
		.expected = {
			LUA_EXPECT_NIL(),
		},
		.expected_count = 1,
	},
};

static const struct lua_kunit_vector depth_api_vectors[] = {
	{
		.id = "DA-find",
		.chunk = "local ok, err = pcall(string.find, 'aaaaaaaab', '^' ..\n"
			 "string.rep('a?', 8) .. 'b'); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DA-find-ok",
		.chunk = "return string.find('aaaaaaab', '^' .. string.rep('a?', 7) ..\n"
			 "'b')",
		.expected = {
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_INTEGER(8),
		},
		.expected_count = 2,
	},
	{
		.id = "DA-plain",
		.chunk = "return string.find('a?a?a?a?a?a?a?a?', string.rep('a?', 8), 1,\n"
			 "true)",
		.expected = {
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_INTEGER(16),
		},
		.expected_count = 2,
	},
	{
		.id = "DA-gmatch",
		.chunk = "local ok, err = pcall(string.gmatch('aaaaaaaab',\n"
			 "string.rep('a?', 8) .. 'b')); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DA-gmatch-ok",
		.chunk = "return string.gmatch('aaaaaaab', string.rep('a?', 7) .. 'b')()",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaab"),
		},
		.expected_count = 1,
	},
	{
		.id = "DA-gfind",
		.chunk = "local ok, err = pcall(string.gfind('aaaaaaaab', string.rep('a?',\n"
			 "8) .. 'b')); return ok, err, string.match('abc123',\n"
			 "'(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DA-gfind-ok",
		.chunk = "return string.gfind('aaaaaaab', string.rep('a?', 7) .. 'b')()",
		.expected = {
			LUA_EXPECT_STRING("aaaaaaab"),
		},
		.expected_count = 1,
	},
	{
		.id = "DA-gsub",
		.chunk = "local ok, err = pcall(string.gsub, 'aaaaaaaab', '^' ..\n"
			 "string.rep('a?', 8) .. 'b', 'X'); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DA-gsub-ok",
		.chunk = "return string.gsub('aaaaaaab', '^' .. string.rep('a?', 7) ..\n"
			 "'b', 'X')",
		.expected = {
			LUA_EXPECT_STRING("X"),
			LUA_EXPECT_INTEGER(1),
		},
		.expected_count = 2,
	},
	{
		.id = "DA-iterator-later",
		.chunk = "local it = string.gmatch('baaaaaaaab', string.rep('a?', 8) ..\n"
			 "'b'); local first = it(); local ok, err = pcall(it); return\n"
			 "first, ok, err, string.match('ab', 'a?b')",
		.expected = {
			LUA_EXPECT_STRING("b"),
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("ab"),
		},
		.expected_count = 4,
	},
	{
		.id = "DA-iterator-retry",
		.invoke = lua_kunit_invoke_depth_retry,
		.expected = {
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("ab"),
		},
		.expected_count = 3,
	},
};

static const struct lua_kunit_vector depth_recovery_vectors[] = {
	{
		.id = "DR-search",
		.chunk = "return string.match(string.rep('a', 16) .. 'b', 'a?b')",
		.expected = {
			LUA_EXPECT_STRING("ab"),
		},
		.expected_count = 1,
	},
	{
		.id = "DR-iteration",
		.invoke = lua_kunit_invoke_depth_iteration,
		.expected = { LUA_EXPECT_BOOLEAN(true) },
		.expected_count = 1,
	},
	{
		.id = "DR-replacements",
		.chunk = "return string.gsub(string.rep('ab', 12), 'a?b', 'X')",
		.expected = {
			LUA_EXPECT_STRING("XXXXXXXXXXXX"),
			LUA_EXPECT_INTEGER(12),
		},
		.expected_count = 2,
	},
	{
		.id = "DR-class",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?['); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("malformed pattern (missing ']')"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-unfinished",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?('); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("unfinished capture"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-invalid",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?%1'); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("invalid capture index"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-close",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?)'); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("invalid pattern capture"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-balance",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?%b'); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("unbalanced pattern"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-frontier",
		.chunk = "local ok, err = pcall(string.match, 'aaa', 'a?a?a?%f'); return\n"
			 "ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("missing"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DR-callback",
		.chunk = "return string.gsub('xxx', 'x', function() local ok, err =\n"
			 "pcall(string.match, 'aaaaaaaa', string.rep('a?', 8)); assert(not\n"
			 "ok and string.find(err, 'pattern recursion limit exceeded', 1,\n"
			 "true)); return string.match('ab', 'a?b') end)",
		.expected = {
			LUA_EXPECT_STRING("ababab"),
			LUA_EXPECT_INTEGER(3),
		},
		.expected_count = 2,
	},
	{
		.id = "DR-table",
		.chunk = "return string.gsub('xxx', 'x', setmetatable({}, {__index =\n"
			 "function() local ok, err = pcall(string.match, 'aaaaaaaa',\n"
			 "string.rep('a?', 8)); assert(not ok and string.find(err,\n"
			 "'pattern recursion limit exceeded', 1, true)); return\n"
			 "string.match('ab', 'a?b') end}))",
		.expected = {
			LUA_EXPECT_STRING("ababab"),
			LUA_EXPECT_INTEGER(3),
		},
		.expected_count = 2,
	},
};

static const struct lua_kunit_vector depth_candidate_vectors[] = {
	{
		.id = "DP-position33",
		.chunk = "local ok, err = pcall(string.match, '', string.rep('()', 33));\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("too many captures"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-normal33",
		.chunk = "local ok, err = pcall(string.match, string.rep('a', 33),\n"
			 "string.rep('(a)', 33)); return ok, err, string.match('abc123',\n"
			 "'(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("too many captures"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-optional64",
		.chunk = "return string.match(string.rep('a', 64) .. 'b', '^' ..\n"
			 "string.rep('a?', 64) .. 'b')",
		.expected = {
			LUA_EXPECT_STRING(LUA_SOURCE_A_16 LUA_SOURCE_A_16
					  LUA_SOURCE_A_16 LUA_SOURCE_A_16 "b"),
		},
		.expected_count = 1,
	},
	{
		.id = "DP-optional65",
		.chunk = "local ok, err = pcall(string.match, string.rep('a', 65) .. 'b',\n"
			 "'^' .. string.rep('a?', 65) .. 'b'); return ok, err,\n"
			 "string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-greedy64",
		.chunk = "return string.match('', '^' .. string.rep('a*', 64) .. '$')",
		.expected = {
			LUA_EXPECT_STRING(""),
		},
		.expected_count = 1,
	},
	{
		.id = "DP-greedy65",
		.chunk = "local ok, err = pcall(string.match, '', '^' .. string.rep('a*',\n"
			 "65) .. '$'); return ok, err, string.match('abc123',\n"
			 "'(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-minimal64",
		.chunk = "return string.match('', '^' .. string.rep('a-', 64) .. '$')",
		.expected = {
			LUA_EXPECT_STRING(""),
		},
		.expected_count = 1,
	},
	{
		.id = "DP-minimal65",
		.chunk = "local ok, err = pcall(string.match, '', '^' .. string.rep('a-',\n"
			 "65) .. '$'); return ok, err, string.match('abc123',\n"
			 "'(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-gsub64",
		.chunk = "return string.gsub('', string.rep('a*', 64), 'X')",
		.expected = {
			LUA_EXPECT_STRING("X"),
			LUA_EXPECT_INTEGER(1),
		},
		.expected_count = 2,
	},
	{
		.id = "DP-gsub65",
		.chunk = "local ok, err = pcall(string.gsub, '', string.rep('a*', 65), 'X');\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-gmatch64",
		.chunk = "return string.gmatch('', string.rep('a*', 64))()",
		.expected = {
			LUA_EXPECT_STRING(""),
		},
		.expected_count = 1,
	},
	{
		.id = "DP-gmatch65",
		.chunk = "local ok, err = pcall(string.gmatch('', string.rep('a*', 65)));\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
	{
		.id = "DP-find64",
		.chunk = "return string.find('', string.rep('a*', 64))",
		.expected = {
			LUA_EXPECT_INTEGER(1),
			LUA_EXPECT_INTEGER(0),
		},
		.expected_count = 2,
	},
	{
		.id = "DP-find65",
		.chunk = "local ok, err = pcall(string.find, '', string.rep('a*', 65));\n"
			 "return ok, err, string.match('abc123', '(%a+)(%d+)')",
		.expected = {
			LUA_EXPECT_BOOLEAN(false),
			LUA_EXPECT_STRING_CONTAINS("pattern recursion limit exceeded"),
			LUA_EXPECT_STRING("abc"),
			LUA_EXPECT_STRING("123"),
		},
		.expected_count = 4,
	},
};

static lua_State *lua_kunit_new_depth_state(struct kunit *test)
{
	lua_State *L = lua_kunit_new_state(test);

	lua_kunit_open_library(test, L, luaopen_base, "");
	lua_kunit_open_library(test, L, luaopen_string_small_depth, LUA_STRLIBNAME);
	return L;
}

static void lua_string_depth_optional_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_depth_state(test);

	lua_kunit_run_vectors(test, L, depth_optional_vectors,
			      ARRAY_SIZE(depth_optional_vectors));
}

static void lua_string_depth_capture_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_depth_state(test);

	lua_kunit_run_vectors(test, L, depth_capture_vectors,
			      ARRAY_SIZE(depth_capture_vectors));
}

static void lua_string_depth_expand_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_depth_state(test);

	lua_kunit_run_vectors(test, L, depth_expand_vectors,
			      ARRAY_SIZE(depth_expand_vectors));
}

static void lua_string_depth_api_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_depth_state(test);

	lua_kunit_run_vectors(test, L, depth_api_vectors,
			      ARRAY_SIZE(depth_api_vectors));
}

static void lua_string_depth_recovery_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_depth_state(test);

	lua_kunit_run_vectors(test, L, depth_recovery_vectors,
			      ARRAY_SIZE(depth_recovery_vectors));
}

static void lua_string_depth_candidate_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);

	lua_kunit_run_vectors(test, L, depth_candidate_vectors,
			      ARRAY_SIZE(depth_candidate_vectors));
}

static void lua_string_depth_restore_test(struct kunit *test)
{
	static const struct {
		const char *src;
		const char *pattern;
		bool matched;
	} paths[] = {
		{ "", "", true },
		{ "a", "$", false },
		{ "", "$", true },
		{ "$", "$a?", true },
		{ "a", "b", false },
		{ "ab", "a?b", true },
		{ "a", "a?a$", true },
		{ "a", "a?b", false },
		{ "a", "()a", true },
		{ "a", "()b", false },
		{ "a", "(a)", true },
		{ "a", "(a)b", false },
		{ "a", "(b)", false },
		{ "a", "(a)%1", false },
		{ "aa", "(a)%1", true },
		{ "a", "%b()", false },
		{ "()", "%b()", true },
		{ "a", "%f[%d]", false },
		{ "a", "%f[%a]a", true },
		{ "aaa", "a*a$", true },
		{ "aaa", "a*a*b", false },
		{ "", "a+", false },
		{ "aaa", "a+a$", true },
		{ "aaa", "a+a+b", false },
		{ "aaa", "a-a$", true },
		{ "aaa", "a-a-b", false },
	};
	lua_State *L = lua_kunit_new_depth_state(test);
	size_t i;
	int status;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		lua_settop(L, 0);
		lua_pushcfunction(L, lua_kunit_match_depth);
		lua_pushstring(L, paths[i].src);
		lua_pushstring(L, paths[i].pattern);
		status = lua_pcall(L, 2, LUA_MULTRET, 0);
		KUNIT_ASSERT_EQ_MSG(test, status, 0, "pattern %s", paths[i].pattern);
		KUNIT_ASSERT_EQ(test, lua_gettop(L), 2);
		KUNIT_EXPECT_EQ_MSG(test, lua_toboolean(L, 1), paths[i].matched,
				    "pattern %s", paths[i].pattern);
		KUNIT_EXPECT_EQ_MSG(test, lua_tointeger(L, 2), 8,
				    "depth leaked for pattern %s", paths[i].pattern);
	}
}

static void lua_string_depth_capture32_test(struct kunit *test)
{
	lua_State *L = lua_kunit_new_behavior_state(test);
	char pattern[97];
	char source[33];
	int status;
	int i;

	memset(source, 'a', 32);
	source[32] = '\0';
	for (i = 0; i < 32; i++)
		memcpy(pattern + i * 3, "(a)", 3);
	pattern[96] = '\0';
	lua_kunit_push_string_function(L, "match");
	lua_pushstring(L, source);
	lua_pushstring(L, pattern);
	status = lua_pcall(L, 2, LUA_MULTRET, 0);
	KUNIT_ASSERT_EQ(test, status, 0);
	KUNIT_ASSERT_EQ(test, lua_gettop(L), 32);
	for (i = 1; i <= 32; i++) {
		KUNIT_ASSERT_EQ(test, lua_type(L, i), LUA_TSTRING);
		KUNIT_EXPECT_STREQ(test, lua_tostring(L, i), "a");
	}
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
	KUNIT_CASE(lua_string_work_behavior_test),
	KUNIT_CASE(lua_string_depth_optional_test),
	KUNIT_CASE(lua_string_depth_capture_test),
	KUNIT_CASE(lua_string_depth_expand_test),
	KUNIT_CASE(lua_string_depth_api_test),
	KUNIT_CASE(lua_string_depth_recovery_test),
	KUNIT_CASE(lua_string_depth_candidate_test),
	KUNIT_CASE(lua_string_depth_restore_test),
	KUNIT_CASE(lua_string_depth_capture32_test),
	{}
};

static struct kunit_suite lua_string_test_suite = {
	.name = "lua-string",
	.test_cases = lua_string_test_cases,
};

kunit_test_suite(lua_string_test_suite);

MODULE_DESCRIPTION("KUnit tests for the in-kernel Lua string library");
MODULE_LICENSE("GPL");
