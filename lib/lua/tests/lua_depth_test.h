/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_DEPTH_TEST_H
#define LUA_DEPTH_TEST_H

#include <linux/lua.h>

int luaopen_string_small_depth(lua_State *L);
int lua_kunit_match_depth(lua_State *L);

#endif
