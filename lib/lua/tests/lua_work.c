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

void lua_work_reset(struct lua_work_observer *observer)
{
	memset(observer->records, 0, sizeof(observer->records));
	observer->used = 0;
	observer->full = false;
}

static void strwork_observer_begin(StrWork *work, enum StrWorkAPI api)
{
	lua_State *L = work->L;
	struct lua_work_observer *observer;
	struct lua_work_record *record;

	work->record = NULL;
	lua_pushlightuserdata(L, &observer_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	observer = lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (!observer)
		return;
	if (observer->limit_override)
		work->budget.remaining = observer->limit;
	if (observer->used == LUA_WORK_RECORDS) {
		observer->full = true;
		return;
	}
	record = &observer->records[observer->used++];
	memset(record, 0, sizeof(*record));
	record->api = api;
	record->initial = work->budget.remaining;
	record->remaining = work->budget.remaining;
	work->record = record;
}

static void strwork_observer_charge(StrWork *work, enum StrWorkKind kind,
				    u64 cost, u64 before, bool allowed)
{
	struct lua_work_record *record = work->record;

	if (!record)
		return;
	record->remaining = work->budget.remaining;
	if (!allowed) {
		if (!record->exceeded) {
			record->rejected_kind = kind;
			record->rejected_cost = cost;
			record->rejected_remaining = before;
		}
		record->exceeded = true;
		return;
	}
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
