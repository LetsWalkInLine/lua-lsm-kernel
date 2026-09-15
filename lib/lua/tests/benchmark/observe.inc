/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the generated private lstrlib copy, never production code. */
#include <linux/minmax.h>

static char bench_observer_key;

void BENCH_OBSERVE(lua_State *L, struct bench_observer *observer)
{
	lua_pushlightuserdata(L, &bench_observer_key);
	if (observer)
		lua_pushlightuserdata(L, observer);
	else
		lua_pushnil(L);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

static void strwork_observer_begin(StrWork *work, enum StrWorkAPI api)
{
	lua_State *L = work->L;
	struct bench_observer *o;
	struct bench_record *r;
	int i, frame = L->ci - L->base_ci;

	work->record = NULL;
	lua_pushlightuserdata(L, &bench_observer_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	o = lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (!o)
		return;
	if (o->used == BENCH_RECORDS) {
		o->full = true;
		return;
	}
	r = &o->records[o->used++];
	memset(r, 0, sizeof(*r));
	r->api = api;
	r->frame = frame;
	r->parent = -1;
	r->rejected_kind = -1;
	r->remaining = work->budget.remaining;
	/* Corpus callbacks complete normally; top-level errors leave the fixture.
	 * Frame ordinals identify the enclosing operation for this bounded corpus.
	 * This is not general VM unwind tracking for arbitrary caught callbacks.
	 */
	for (i = o->used - 2; i >= 0; i--) {
		if (!o->records[i].finished && o->records[i].frame < frame) {
			r->parent = i;
			break;
		}
	}
	work->record = r;
}

static void strwork_observer_charge(StrWork *work, enum StrWorkKind kind,
				    u64 cost, u64 before, bool allowed)
{
	struct bench_record *r = work->record;

	if (!r)
		return;
	r->remaining = work->budget.remaining;
	if (!allowed) {
		if (!r->exceeded) {
			r->rejected_kind = kind;
			r->rejected_cost = cost;
			r->rejected_remaining = before;
		}
		r->exceeded = true;
		return;
	}
	r->overflow |= strwork_accumulate(&r->count[kind], cost);
	r->overflow |= strwork_accumulate(&r->total, cost);
}

static void strwork_end(StrWork *work)
{
	struct bench_record *r = work->record;

	if (r)
		r->finished = true;
}

static void bench_depth(StrWork *work, unsigned int depth, bool admitted)
{
	struct bench_record *r = work->record;

	if (!r)
		return;
	if (admitted)
		r->peak_admitted = max(r->peak_admitted, depth);
	else
		r->peak_attempted = max(r->peak_attempted, depth);
}
