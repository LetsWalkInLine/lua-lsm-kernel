/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_WORK_TEST_H
#define LUA_WORK_TEST_H

#include <linux/lua.h>
#include "../lstrwork.h"

#define LUA_WORK_RECORDS 32

struct lua_work_record {
	u64 total;
	u64 initial;
	u64 remaining;
	u64 rejected_cost;
	u64 rejected_remaining;
	enum StrWorkAPI api;
	bool exceeded;
	bool finished;
};

struct lua_work_observer {
	struct lua_work_record records[LUA_WORK_RECORDS];
	unsigned int used;
	bool full;
	/* C-only configuration; clearing records must preserve it. */
	bool limit_override;
	u64 limit;
};

int luaopen_string_work(lua_State *L);
void lua_work_observe(lua_State *L, struct lua_work_observer *observer);
void lua_work_reset(struct lua_work_observer *observer);

#endif
