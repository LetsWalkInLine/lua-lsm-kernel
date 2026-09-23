// SPDX-License-Identifier: GPL-2.0-only

#include "lua_depth_test.h"

/* Exercise the real implementation without changing the policy library. */
#define LUA_PATTERN_MAXDEPTH 8
#define luaopen_string luaopen_string_small_depth
#include "../lstrlib.c"

/* Called under lua_pcall, only in a private KUnit state. */
int lua_kunit_match_depth(lua_State *L)
{
	MatchState ms;
	StrWork work;
	const char *src;
	const char *pattern;
	const char *result;
	size_t len;

	src = luaL_checklstring(L, 1, &len);
	pattern = luaL_checkstring(L, 2);
	strwork_begin(L, &work, SW_MATCH);
	ms.L = L;
	strwork_bind(&ms, &work);
	ms.src_init = src;
	ms.src_end = src + len;
	ms.level = 0;
	ms.matchdepth = LUA_PATTERN_MAXDEPTH;
	result = match(&ms, src, pattern);
	lua_pushboolean(L, !!result);
	lua_pushinteger(L, ms.matchdepth);
	strwork_end(&work);
	return 2;
}
