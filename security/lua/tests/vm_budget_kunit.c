// SPDX-License-Identifier: GPL-2.0-only
/* Production VM and dispatchers. Dangerous programs run only in watchdog guests. */
#include <kunit/test.h>
#include "test_common.h"
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include "../lsm.h"
#include "../lsm_defs.h"
#include "../../../lib/lua/ldo.h"

#define META "author='tests',description='budget',license='GPL-2.0',version=1,"

static void register_code(struct kunit *test, const char *prefix,
			  const char *body)
{
	char *code = kunit_kmalloc(test, 8192, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, code);
	snprintf(code, 8192,
		 "%s local M={name='budget_first'," META "hit=0};"
		 "local function f() M.hit=M.hit+1;%s end;"
		 "M.file_open=f;M.inode_xattr_skipcap=f;M.ismaclabel=f;"
		 "M.inode_need_killpriv=f;M.inode_post_setxattr=f;return M\n",
		 prefix, body);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
}

static void remove_policy(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("budget_first"), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
}

static void clean_state(struct kunit *test)
{
	lua_State *L = lvm_get();

	KUNIT_ASSERT_NOT_NULL(test, L);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 0);
	KUNIT_EXPECT_EQ(test, G(L)->budget_active, 0);
	KUNIT_EXPECT_EQ(test, G(L)->budget_expired, 0);
	KUNIT_EXPECT_EQ(test, G(L)->deadline, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, lua_gethook(L), NULL);
	lua_lsm_get_current_env(L);
	KUNIT_EXPECT_EQ(test, lua_type(L, -1), LUA_TBOOLEAN);
	KUNIT_EXPECT_FALSE(test, lua_toboolean(L, -1));
	lua_pop(L, 1);
	lvm_put(L);
	KUNIT_EXPECT_EQ(test,
			atomic_read(&lua_lsm_task(current)->lvm->refcount), 0);
}

static void reliable_termination(struct kunit *test)
{
	static const char *const programs[] = {
		"while true do end",
		"debug.sethook();while true do end",
		"debug.sethook(function()end,'',1);while true do end",
		"pcall(function()while true do end end);M.escaped=true;return true",
		"while true do pcall(function()error({})end) end",
		"xpcall(function()while true do end end,function()M.escaped=true;return true end);M.escaped=true",
		"xpcall(function()error('x')end,function()while true do end end);M.escaped=true",
		"xpcall(function()error('x')end,function()while true do pcall(function()error({})end)end end)",
		"debug.sethook(function()while true do end end,'',1);return true",
		"local co=coroutine.create(function()pcall(function()while true do end end);M.escaped=true end);coroutine.resume(co);M.escaped=true;return true",
		"coroutine.wrap(function()while true do end end)();M.escaped=true",
		"local a=coroutine.create(function()while true do coroutine.yield()end end);"
		"local b=coroutine.create(function()while true do coroutine.yield()end end);"
		"while true do assert(coroutine.resume(a));assert(coroutine.resume(b))end",
		"local co=coroutine.create(function()while true do end end);debug.sethook(co,function()end,'',1);coroutine.resume(co);M.escaped=true",
		"string.gsub('x','.',function()pcall(function()while true do end end);M.escaped=true;return '' end);M.escaped=true",
		"pcall(function()local a=newproxy(true);getmetatable(a).__gc=function()while true do end end;"
		"a=nil;collectgarbage('collect')end);M.escaped=true",
		"setfenv(0,{});while true do end",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(programs); i++) {
		u64 start, elapsed;
		lua_State *L;
		const void *globals;

		L = lvm_get();
		globals = lua_topointer(L, LUA_GLOBALSINDEX);
		lvm_put(L);

		/* The first call times out; shared module state makes the next succeed. */
		{
			char *body = kunit_kmalloc(test, 4096, GFP_KERNEL);

			KUNIT_ASSERT_NOT_NULL(test, body);
			snprintf(
				body, 4096,
				"if M.hit>1 then assert(not M.escaped);return true end;%s",
				programs[i]);
			register_code(test, "", body);
		}
		start = ktime_get_ns();
		KUNIT_EXPECT_EQ_MSG(test, lua_lsm_file_open(NULL), -EIO,
				    "program %d", i);
		elapsed = ktime_get_ns() - start;
		KUNIT_EXPECT_GE_MSG(test, elapsed, lua_lsm_budget_ns(),
				    "early ordinary error %d", i);
		kunit_info(test, "timeout program=%d elapsed_ns=%llu\n", i,
			   elapsed);
		clean_state(test);
		KUNIT_EXPECT_EQ_MSG(test, lua_lsm_file_open(NULL), 0,
				    "reuse %d", i);
		/* Read without invoking a module metatable. */
		L = lvm_get();
		KUNIT_EXPECT_PTR_EQ(test, lua_topointer(L, LUA_GLOBALSINDEX),
				    globals);
		lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
		lua_getfield(L, -1, "budget_first");
		lua_getfield(L, -1, "escaped");
		KUNIT_EXPECT_FALSE_MSG(test, lua_toboolean(L, -1), "escaped %d",
				       i);
		lua_settop(L, 0);
		lvm_put(L);
		remove_policy(test);
	}
}

static void lazy_lookup_and_categories(struct kunit *test)
{
	static const char second[] =
		"return {name='budget_second'," META
		"file_open=function()error('later module ran')end}\n";
	static const char *const prefix[] = {
		"if current then while true do end end;",
		"if current then return setmetatable({}, {__index=function()while true do end end})end;",
		"if current then local f=function()end;setfenv(f,setmetatable({},"
		"{__newindex=function()while true do end end}));return {file_open=f}end;",
	};
	int i;
	lua_State *L;

	for (i = 0; i < ARRAY_SIZE(prefix); i++) {
		register_code(test, prefix[i], "return true");
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		clean_state(test);
		remove_policy(test);
	}
	register_code(test, "", "while true do end");
	KUNIT_ASSERT_EQ(test,
			lua_lsm_module_register(second, sizeof(second) - 1), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_need_killpriv(NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_xattr_skipcap("x"), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_ismaclabel("x"), 1);
	lua_lsm_inode_post_setxattr(NULL, "x", "v", 1, 0);
	L = lvm_get();
	lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
	lua_pushliteral(L, "budget_second");
	lua_rawget(L, -2);
	KUNIT_EXPECT_TRUE(test, lua_isnil(L, -1));
	lua_settop(L, 0);
	lvm_put(L);
	clean_state(test);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("budget_second"), 0);
	remove_policy(test);
}

/* C helper returns after the deadline: no following Lua instruction may run. */
static unsigned int overrun_calls;

static int overrun(lua_State *L)
{
	overrun_calls++;
	G(L)->deadline = ktime_get_ns() - 1;
	return 0;
}

static u64 observed_budget;
static int observe(lua_State *L)
{
	observed_budget = G(L)->deadline - ktime_get_ns();
	return 0;
}

static int spend(lua_State *L)
{
	u64 until = ktime_get_ns() + lua_lsm_budget_ns() * 3 / 5;

	while ((s64)(ktime_get_ns() - until) < 0)
		cpu_relax();
	return 0;
}

static void set_helper(lua_CFunction fn)
{
	lua_State *L = lvm_get();

	if (fn)
		lua_pushcfunction(L, fn);
	else
		lua_pushnil(L);
	lua_setglobal(L, "budget_helper");
	lvm_put(L);
}

static void shared_deadline_and_c_return(struct kunit *test)
{
	static const char *const bodies[] = {
		"budget_helper();M.escaped=true;return true",
		"pcall(budget_helper);M.escaped=true;return true",
		"xpcall(budget_helper,function()M.escaped=true end);M.escaped=true",
		"coroutine.resume(coroutine.create(function()budget_helper();M.escaped=true end));M.escaped=true",
		"string.gsub('a','.',function()budget_helper();M.escaped=true end);M.escaped=true",
	};
	int i;

	set_helper(overrun);
	for (i = 0; i < ARRAY_SIZE(bodies); i++) {
		lua_State *L;

		register_code(test, "", bodies[i]);
		overrun_calls = 0;
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		KUNIT_EXPECT_EQ_MSG(test, overrun_calls, 1U,
				    "helper invocation %d", i);
		L = lvm_get();
		lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
		lua_getfield(L, -1, "budget_first");
		lua_getfield(L, -1, "escaped");
		KUNIT_EXPECT_FALSE(test, lua_toboolean(L, -1));
		lua_settop(L, 0);
		lvm_put(L);
		clean_state(test);
		remove_policy(test);
	}
	set_helper(spend);
	/* Loader and callback each fit, but together exceed the same deadline. */
	register_code(test, "if current then budget_helper()end;",
		      "budget_helper();return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL),
			0); /* already loaded: one spend */
	clean_state(test);
	remove_policy(test);
	/* Two modules each receive a fresh budget, rather than sharing the hook total. */
	register_code(test, "", "budget_helper();return true");
	{
		static const char second[] =
			"return {name='budget_second'," META
			"file_open=function()budget_helper();return true end}\n";
		KUNIT_ASSERT_EQ(
			test,
			lua_lsm_module_register(second, sizeof(second) - 1), 0);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
		KUNIT_ASSERT_EQ(test,
				lua_lsm_module_unregister("budget_second"), 0);
	}
	remove_policy(test);
	set_helper(NULL);
}

/* Scheduling delay is charged. This helper is used only in sleepable task context. */
static int sleep_past_deadline(lua_State *L)
{
	lua_budget_begin(L, NSEC_PER_MSEC);
	msleep(20);
	return 0;
}

static void scheduling_delay(struct kunit *test)
{
	lua_State *L = lvm_get();
	int status;

	status = lua_cpcall(L, sleep_past_deadline, NULL);
	KUNIT_EXPECT_EQ(test, status, LUA_ERRTIME);
	KUNIT_EXPECT_EQ(test, lua_budget_end(L), LUA_ERRTIME);
	lua_settop(L, 0);
	lvm_put(L);
	clean_state(test);
}

static lua_Alloc saved_alloc;
static void *saved_ud;
static bool refuse_growth;
static unsigned int attempted_growth;
static void *timeout_alloc(void *ud, void *ptr, size_t old, size_t size)
{
	if (refuse_growth && size > old) {
		attempted_growth++;
		return NULL;
	}
	return saved_alloc(saved_ud, ptr, old, size);
}
static int arm_timeout_oom(lua_State *L)
{
	refuse_growth = true;
	G(L)->deadline = ktime_get_ns() - 1;
	return 0;
}
static int protected_oom(lua_State *L)
{
	lua_settop(L, 0);
	lua_pushcfunction(L, arm_timeout_oom);
	lua_budget_begin(L, NSEC_PER_SEC);
	lua_pcall(L, 0, 0, 0);
	return 0;
}
static void timeout_needs_no_allocation(struct kunit *test)
{
	lua_State *L = lvm_get();
	int status;

	refuse_growth = false;
	attempted_growth = 0;
	saved_alloc = lua_getallocf(L, &saved_ud);
	lua_setallocf(L, timeout_alloc, NULL);
	status = lua_cpcall(L, protected_oom, NULL);
	refuse_growth = false;
	lua_setallocf(L, saved_alloc, saved_ud);
	KUNIT_EXPECT_EQ(test, status, LUA_ERRTIME);
	KUNIT_EXPECT_EQ(test, attempted_growth, 0U);
	KUNIT_EXPECT_EQ(test, lua_budget_end(L), LUA_ERRTIME);
	lua_settop(L, 0);
	lvm_put(L);
	clean_state(test);
}

struct softirq_check {
	struct tasklet_struct tasklet;
	struct completion done;
	int result;
	u64 budget;
};
static void atomic_callback(struct tasklet_struct *t)
{
	struct softirq_check *s = from_tasklet(s, t, tasklet);
	lua_State *L = lvm_get();

	lua_pushcfunction(L, observe);
	lua_setglobal(L, "budget_helper");
	lvm_put(L);
	s->result = lua_lsm_file_open(NULL);
	s->budget = observed_budget;
	complete(&s->done);
}

static void contexts(struct kunit *test)
{
	struct softirq_check s;
	u64 budget;
	int result;
	unsigned long flags;

	set_helper(observe);
	register_code(test, "", "budget_helper();return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	KUNIT_EXPECT_GT(test, observed_budget, lua_lsm_budget_ns() / 2);
	preempt_disable();
	result = lua_lsm_file_open(NULL);
	budget = observed_budget;
	preempt_enable();
	KUNIT_EXPECT_EQ(test, result, 0);
	KUNIT_EXPECT_LE(test, budget, LUA_LSM_BUDGET_ATOMIC_NS);
	rcu_read_lock();
	result = lua_lsm_file_open(NULL);
	budget = observed_budget;
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, result, 0);
	KUNIT_EXPECT_LE(test, budget, LUA_LSM_BUDGET_ATOMIC_NS);
	local_irq_save(flags);
	result = lua_lsm_file_open(NULL);
	budget = observed_budget;
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, result, 0);
	KUNIT_EXPECT_LE(test, budget, LUA_LSM_BUDGET_ATOMIC_NS);
	init_completion(&s.done);
	tasklet_setup(&s.tasklet, atomic_callback);
	tasklet_schedule(&s.tasklet);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s.done, 10 * HZ),
			0UL);
	tasklet_kill(&s.tasklet);
	KUNIT_EXPECT_EQ(test, s.result, 0);
	KUNIT_EXPECT_LE(test, s.budget, LUA_LSM_BUDGET_ATOMIC_NS);
	remove_policy(test);
	/* Reliable timeout and recovery in both preempt-disabled task and softirq. */
	register_code(test, "", "while true do end");
	preempt_disable();
	result = lua_lsm_file_open(NULL);
	preempt_enable();
	KUNIT_EXPECT_EQ(test, result, -EIO);
	init_completion(&s.done);
	tasklet_schedule(&s.tasklet);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s.done, 10 * HZ),
			0UL);
	tasklet_kill(&s.tasklet);
	KUNIT_EXPECT_EQ(test, s.result, -EIO);
	clean_state(test);
	remove_policy(test);
	set_helper(NULL);
}

/* Pure bytecode makes the exact interval observable without a clock observer. */
static unsigned int instructions;
static void expire_from_hook(lua_State *L, lua_Debug *ar)
{
	if (++instructions == 1)
		G(L)->deadline = ktime_get_ns() - 1;
}
static int checkpoint_run(lua_State *L)
{
	lua_settop(L, 0);
	{
		int status = luaL_loadstring(L, "while true do end\n");

		if (status)
			return lua_rethrow(L, status);
	}
	lua_budget_begin(L, NSEC_PER_SEC);
	lua_sethook(L, expire_from_hook, LUA_MASKCOUNT, 1);
	lua_call(L, 0, 0);
	return 0;
}
static void independent_checkpoint(struct kunit *test)
{
	lua_State *L = lvm_get();
	int status;

	instructions = 0;
	status = lua_cpcall(L, checkpoint_run, NULL);
	KUNIT_EXPECT_EQ_MSG(test, status, LUA_ERRTIME, "error: %s",
			    lua_tostring(L, -1));
	KUNIT_EXPECT_EQ(test, instructions, LUA_BUDGET_INTERVAL - 1);
	KUNIT_EXPECT_EQ(test, lua_budget_end(L), LUA_ERRTIME);
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, 0);
	lvm_put(L);
	clean_state(test);
}

static int short_return(lua_State *L)
{
	int status;

	lua_settop(L, 0);
	status = luaL_loadstring(L, "return true\n");
	if (status)
		return lua_rethrow(L, status);
	lua_budget_begin(L, NSEC_PER_SEC);
	lua_sethook(L, expire_from_hook, LUA_MASKCOUNT, 1);
	lua_call(L, 0, 0);
	return 0;
}
static void exit_before_interval(struct kunit *test)
{
	lua_State *L = lvm_get();

	instructions = 0;
	KUNIT_EXPECT_EQ(test, lua_cpcall(L, short_return, NULL), LUA_ERRTIME);
	KUNIT_EXPECT_LT(test, instructions, LUA_BUDGET_INTERVAL);
	KUNIT_EXPECT_GT(test, instructions, 0U);
	KUNIT_EXPECT_EQ(test, lua_budget_end(L), LUA_ERRTIME);
	lua_sethook(L, NULL, 0, 0);
	lua_settop(L, 0);
	lvm_put(L);
	clean_state(test);
}

static int expiry_run(lua_State *L)
{
	lua_settop(L, 0);
	lua_getglobal(L, "budget_loop");
	lua_budget_begin(L, NSEC_PER_MSEC);
	lua_call(L, 0, 0);
	return 0;
}
static void deadline_expiry(struct kunit *test)
{
	lua_State *L = lvm_get();

	KUNIT_ASSERT_EQ(test, luaL_loadstring(L, "while true do end\n"), 0);
	lua_setglobal(L, "budget_loop");
	{
		u64 start = ktime_get_ns(), elapsed;
		int status = lua_cpcall(L, expiry_run, NULL);

		elapsed = ktime_get_ns() - start;
		KUNIT_EXPECT_EQ(test, status, LUA_ERRTIME);
		KUNIT_EXPECT_EQ(test, lua_budget_end(L), LUA_ERRTIME);
		lua_settop(L, 0);
		KUNIT_EXPECT_GE(test, elapsed, (u64)NSEC_PER_MSEC);
	}
	lua_pushnil(L);
	lua_setglobal(L, "budget_loop");
	lvm_put(L);
	clean_state(test);
}

static void postpone_after_timeout(struct kunit *test)
{
	static const char policy[] =
		"return {name='budget_first'," META
		"file_free_security=function(file)file.marker=42;while true do end end}\n";
	struct file *file = kunit_kzalloc(test, sizeof(*file), GFP_KERNEL);
	struct lua_lsm_object *object;

	KUNIT_ASSERT_NOT_NULL(test, file);
	file->f_security = kunit_kzalloc(
		test, lua_lsm_blob_sizes.lbs_file + sizeof(*object),
		GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, file->f_security);
	object = lua_lsm_file(file);
	KUNIT_ASSERT_EQ(test,
			lua_lsm_module_register(policy, sizeof(policy) - 1), 0);
	lua_lsm_file_free_security(file);
	KUNIT_EXPECT_EQ(test, atomic_read(&object->dict.count), 0);
	clean_state(test);
	remove_policy(test);
}

static struct kunit_case cases[] = {
	KUNIT_CASE(reliable_termination),
	KUNIT_CASE(lazy_lookup_and_categories),
	KUNIT_CASE(shared_deadline_and_c_return),
	KUNIT_CASE(scheduling_delay),
	KUNIT_CASE(timeout_needs_no_allocation),
	KUNIT_CASE(contexts),
	KUNIT_CASE(independent_checkpoint),
	KUNIT_CASE(exit_before_interval),
	KUNIT_CASE(deadline_expiry),
	KUNIT_CASE(postpone_after_timeout),
	{}
};
static struct kunit_suite suite = {
	.name = "lua_lsm_vm_budget",
	.init = lua_lsm_test_init,
	.test_cases = cases,
};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
