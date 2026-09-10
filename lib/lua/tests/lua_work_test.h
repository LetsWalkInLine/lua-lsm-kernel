/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_WORK_TEST_H
#define LUA_WORK_TEST_H

#include <linux/lua.h>
#include "../lstrwork.h"

#define LUA_WORK_RECORDS 32

struct lua_work_record {
	u64 count[SW_KINDS];
	u64 total;
	enum StrWorkAPI api;
	bool overflow;
	bool finished;
};

struct lua_work_observer {
	struct lua_work_record records[LUA_WORK_RECORDS];
	unsigned int used;
	bool full;
};

int luaopen_string_work(lua_State *L);
void lua_work_observe(lua_State *L, struct lua_work_observer *observer);

#endif
