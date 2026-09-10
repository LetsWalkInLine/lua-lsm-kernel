// SPDX-License-Identifier: GPL-2.0-only

#define LUA_STRING_WORK_TEST
#include "lua_work_test.h"

#define luaopen_string luaopen_string_work
#include "../lstrlib.c"

/* Only private KUnit states install this registry entry. */
static char observer_key;

void lua_work_observe(lua_State *L, struct lua_work_observer *observer)
{
	lua_pushlightuserdata(L, &observer_key);
	if (observer)
		lua_pushlightuserdata(L, observer);
	else
		lua_pushnil(L);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

static void strwork_begin(lua_State *L, StrWork *work, enum StrWorkAPI api)
{
	struct lua_work_observer *observer;
	struct lua_work_record *record;

	work->record = NULL;
	lua_pushlightuserdata(L, &observer_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	observer = lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (!observer)
		return;
	if (observer->used == LUA_WORK_RECORDS) {
		observer->full = true;
		return;
	}
	record = &observer->records[observer->used++];
	memset(record, 0, sizeof(*record));
	record->api = api;
	work->record = record;
	strwork_charge(work, SW_OPERATION, 1);
}

static void strwork_charge(StrWork *work, enum StrWorkKind kind, u64 cost)
{
	struct lua_work_record *record = work->record;

	if (!record)
		return;
	/* Evaluate both additions even if an earlier event already overflowed. */
	record->overflow |= strwork_accumulate(&record->count[kind], cost);
	record->overflow |= strwork_accumulate(&record->total, cost);
}

static void strwork_end(StrWork *work)
{
	struct lua_work_record *record = work->record;

	if (record)
		record->finished = true;
}
