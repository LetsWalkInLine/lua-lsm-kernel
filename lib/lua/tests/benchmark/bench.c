// SPDX-License-Identifier: GPL-2.0-only
/* Linked only by prepare.py in an isolated measurement source overlay. */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "observe.h"

struct bench_value {
	int type;
	long long number;
	const char *string;
	size_t len;
};
struct bench_sample {
	const char *id, *chunk, *guarded_error, *unlimited_error;
	const struct bench_value *values;
	unsigned int count;
};
#include "samples.h"

static char mode;
static unsigned int boot_id;
static bool pilot;

static int __init bench_mode(char *str)
{
	if (str && str[0] && !str[1] && strchr("ABCDH", str[0]))
		mode = str[0];
	return 1;
}
__setup("lua_bench=", bench_mode);

static int __init bench_boot(char *str)
{
	if (kstrtouint(str, 10, &boot_id))
		mode = 0;
	return 1;
}
__setup("lua_bench_boot=", bench_boot);

static int __init bench_pilot(char *str)
{
	pilot = true;
	return 1;
}
__setup("lua_bench_pilot", bench_pilot);

static void *bench_alloc(void *ud, void *ptr, size_t old, size_t size)
{
	if (!size) {
		kfree(ptr);
		return NULL;
	}
	return krealloc(ptr, size, GFP_KERNEL);
}

static void observe(lua_State *L, struct bench_observer *o)
{
	if (mode == 'B')
		bench_observe_b(L, o);
	else if (mode == 'D')
		bench_observe_d(L, o);
}

static int open_libs(lua_State *L)
{
	lua_CFunction open;

	switch (mode) {
	case 'A':
		open = luaopen_string_bench_a;
		break;
	case 'B':
		open = luaopen_string_bench_b;
		break;
	case 'D':
		open = luaopen_string_bench_d;
		break;
	case 'H':
		open = luaopen_string_bench_h;
		break;
	default:
		open = luaopen_string;
		break;
	}
	lua_pushcfunction(L, luaopen_base);
	lua_pushliteral(L, "");
	lua_call(L, 1, 0);
	lua_pushcfunction(L, open);
	lua_pushliteral(L, LUA_STRLIBNAME);
	lua_call(L, 1, 0);
	return 0;
}

static bool validate(lua_State *L, const struct bench_sample *s,
		     const char *error, int status)
{
	unsigned int i;

	if (error)
		return status == LUA_ERRRUN && lua_gettop(L) == 3 &&
		       lua_type(L, 3) == LUA_TSTRING &&
		       strstr(lua_tostring(L, 3), error);
	if (status || lua_gettop(L) != 2 + s->count)
		return false;
	for (i = 0; i < s->count; i++) {
		const struct bench_value *v = &s->values[i];
		int slot = 3 + i;
		size_t len;
		const char *str;

		if (lua_type(L, slot) != v->type)
			return false;
		switch (v->type) {
		case LUA_TSTRING:
			str = lua_tolstring(L, slot, &len);
			if (len != v->len || memcmp(str, v->string, len))
				return false;
			break;
		case LUA_TNUMBER:
			if (lua_tonumber(L, slot) != v->number)
				return false;
			break;
		case LUA_TBOOLEAN:
			if (lua_toboolean(L, slot) != v->number)
				return false;
			break;
		}
	}
	return true;
}

static void emit_work(char *line, const struct bench_sample *s,
		      struct bench_observer *o, unsigned int repeat, int status)
{
	unsigned int i, k;
	int n;

	pr_info("M5O %c %s %u %u %u %u %d\n", mode, s->id, boot_id,
		repeat, o->used, o->full, status);
	for (i = 0; i < o->used; i++) {
		struct bench_record *r = &o->records[i];

		n = scnprintf(line, 4096,
			"M5W %c %s %u %u %u %d %d %u %u %llu %llu %u %d %llu %llu %u %u",
			mode, s->id, boot_id, repeat, i, r->api, r->parent,
			r->peak_admitted, r->peak_attempted, r->total, r->remaining,
			r->exceeded, r->rejected_kind, r->rejected_cost,
			r->rejected_remaining, r->finished, r->overflow);
		for (k = 0; k < SW_KINDS; k++)
			n += scnprintf(line + n, 4096 - n, " %llu", r->count[k]);
		pr_info("%s\n", line);
	}
}

static int run_sample(const struct bench_sample *s)
{
	struct bench_observer *o = NULL;
	lua_State *L = NULL;
	char *line;
	u64 *times, before;
	unsigned int block, blocks, i, n, warmup, repetitions;
	int status = 0, ret = -EINVAL;
	bool counting = mode == 'B' || mode == 'D';
	const char *error = (mode == 'C' || mode == 'D') ?
		s->guarded_error : s->unlimited_error;

	line = kmalloc(4096, GFP_KERNEL);
	times = kmalloc_array(100, sizeof(*times), GFP_KERNEL);
	if (counting)
		o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!line || !times || (counting && !o))
		goto out;
	blocks = counting || error || pilot ? 1 : 20;
	repetitions = counting ? 2 : pilot ? 10 : error ? 50 : 100;
	/* 5 warmups x 20 fresh blocks = 100 per successful sample per boot. */
	warmup = counting ? 0 : pilot ? 2 : 5;
	for (block = 0; block < blocks; block++) {
		L = lua_newstate(bench_alloc, NULL);
		if (!L)
			goto out;
		lua_pushcfunction(L, open_libs);
		if (lua_pcall(L, 0, 0, 0))
			goto out;
		if (luaL_loadbuffer(L, s->chunk, strlen(s->chunk), s->id) ||
		    lua_pcall(L, 0, 2, 0) || !lua_isfunction(L, 1))
			goto out;
		if (!lua_checkstack(L, 256))
			goto out;
		lua_gc(L, LUA_GCCOLLECT, 0);
		for (i = 0; i < warmup + repetitions; i++) {
			lua_settop(L, 2);
			if (lua_isfunction(L, 2)) {
				lua_pushvalue(L, 2);
				if (lua_pcall(L, 0, 0, 0))
					goto out;
			}
			if (counting) {
				memset(o, 0, sizeof(*o));
				observe(L, o);
			}
			lua_pushvalue(L, 1);
			if (counting) {
				status = lua_pcall(L, 0, LUA_MULTRET, 0);
				observe(L, NULL);
			} else {
				before = ktime_get_ns();
				status = lua_pcall(L, 0, LUA_MULTRET, 0);
				before = ktime_get_ns() - before;
				if (i >= warmup)
					times[i - warmup] = before;
			}
			if (!validate(L, s, error, status))
				goto out;
			if (counting) {
				unsigned int k;

				if (o->full)
					goto out;
				for (k = 0; k < o->used; k++)
					if (o->records[k].overflow)
						goto out;
				emit_work(line, s, o, i, status);
			}
		}
		if (!counting) {
			n = scnprintf(line, 4096, "M5T %c %s %u %u %u %u %d",
				mode, s->id, boot_id, block, warmup, repetitions, status);
			for (i = 0; i < repetitions; i++)
				n += scnprintf(line + n, 4096 - n, " %llu", times[i]);
			pr_info("%s\n", line);
		}
		/* Check reuse outside observations/timing, including after errors. */
		lua_settop(L, 0);
		if (luaL_loadstring(L, "assert(string.match('ab','^a?b$')=='ab')") ||
		    lua_pcall(L, 0, 0, 0))
			goto out;
		lua_close(L);
		L = NULL;
		cond_resched();
	}
	ret = 0;
out:
	if (ret) {
		pr_err("M5BENCH FAIL %c %s status=%d stack=%d error=%s\n",
			mode, s->id, status, L ? lua_gettop(L) : -1,
			L && lua_isstring(L, -1) ? lua_tostring(L, -1) : "none");
	}
	if (L)
		lua_close(L);
	kfree(o);
	kfree(times);
	kfree(line);
	return ret;
}

static int __init bench_init(void)
{
	unsigned int i, index;

	if (!mode)
		return 0;
	/* Let boot-time TSC refinement complete before any measured operation. */
	msleep(2000);
	pr_info("M5BENCH BEGIN mode=%c boot=%u pilot=%u cases=%zu task=%s pid=%d\n",
		mode, boot_id, pilot, ARRAY_SIZE(samples), current->comm,
		task_pid_nr(current));
	for (i = 0; i < ARRAY_SIZE(samples); i++) {
		index = (i + boot_id * 7) % ARRAY_SIZE(samples);
		if (run_sample(&samples[index]))
			return 0; /* Host requires END PASS; failure is never silently accepted. */
	}
	pr_info("M5BENCH END PASS mode=%c boot=%u\n", mode, boot_id);
	return 0;
}
late_initcall(bench_init);
