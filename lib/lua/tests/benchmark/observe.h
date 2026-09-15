/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_BENCH_OBSERVE_H
#define LUA_BENCH_OBSERVE_H
#include <linux/lua.h>
#include "../../lstrwork.h"

#define BENCH_RECORDS 32
struct bench_record {
	u64 count[SW_KINDS], total, remaining, rejected_cost, rejected_remaining;
	unsigned int peak_admitted, peak_attempted;
	int api, parent, frame, rejected_kind;
	bool finished, exceeded, overflow;
};
struct bench_observer {
	struct bench_record records[BENCH_RECORDS];
	unsigned int used;
	bool full;
};
int luaopen_string_bench_a(lua_State *L);
int luaopen_string_bench_b(lua_State *L);
int luaopen_string_bench_d(lua_State *L);
int luaopen_string_bench_h(lua_State *L);
void bench_observe_b(lua_State *L, struct bench_observer *observer);
void bench_observe_d(lua_State *L, struct bench_observer *observer);
#endif
