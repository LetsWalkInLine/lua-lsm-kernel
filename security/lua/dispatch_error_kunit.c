// SPDX-License-Identifier: GPL-2.0-only
/* Exercise production dispatchers; all injection stays in this test object. */
#include <kunit/test.h>
#include "tests/test-common.h"
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include "lsm.h"
#include "lsm_defs.h"
#include "lua_object.h"

#define META "author='tests',description='dispatch errors',license='GPL-2.0',version=1,"
#define HOOKS "M.file_open=f;M.inode_xattr_skipcap=f;M.ismaclabel=f;" \
	"M.audit_rule_known=f;M.inode_need_killpriv=f;M.audit_rule_match=f;" \
	"M.capget=f;M.inode_init_security=f;M.inode_getsecurity=f;" \
	"M.inode_listsecurity=f;M.inode_post_setxattr=f;" \
	"M.vm_enough_memory=f;M.task_prctl=f;M.fs_context_parse_param=f;M.file_mprotect=f;"

static void register_body(struct kunit *test, const char *body)
{
	char *code = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, code);
	snprintf(code, 4096,
		"local M={name='dispatch_first'," META "hit=0};"
		"local function f(...) M.hit=M.hit+1;%s end;" HOOKS "return M\n", body);

	KUNIT_ASSERT_NOT_NULL(test, code);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
}

struct snapshot {
	const void *globals;
	lua_Hook hook;
	int mask, count;
};

static struct snapshot snapshot(struct kunit *test)
{
	lua_State *L = lvm_get();
	struct snapshot s = {};

	KUNIT_EXPECT_NOT_NULL(test, L);
	if (!L)
		return s;
	s.globals = lua_topointer(L, LUA_GLOBALSINDEX);
	s.hook = lua_gethook(L);
	s.mask = lua_gethookmask(L);
	s.count = lua_gethookcount(L);
	KUNIT_EXPECT_EQ(test, lua_gettop(L), 0);
	lua_lsm_get_current_env(L);
	KUNIT_EXPECT_EQ(test, lua_type(L, -1), LUA_TBOOLEAN);
	KUNIT_EXPECT_EQ(test, lua_toboolean(L, -1), 0);
	lua_pop(L, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&lua_lsm_task(current)->lvm->refcount), 1);
	lvm_put(L);
	KUNIT_EXPECT_EQ(test, atomic_read(&lua_lsm_task(current)->lvm->refcount), 0);
	return s;
}

static void check_state(struct kunit *test, struct snapshot before)
{
	struct snapshot after = snapshot(test);

	KUNIT_EXPECT_PTR_EQ(test, after.globals, before.globals);
	KUNIT_EXPECT_TRUE(test, after.hook == before.hook);
	KUNIT_EXPECT_EQ(test, after.mask, before.mask);
	KUNIT_EXPECT_EQ(test, after.count, before.count);
}

static void unregister_first(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("dispatch_first"), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
}

static void errno_errors(struct kunit *test)
{
	const char *bodies[] = {
		"error('ordinary')", "error({})", "error(42)",
		"string.byte({})",
		"debug.traceback=function() error('handler') end;error('original')",
	};
	struct snapshot before = snapshot(test);
	int i;

	for (i = 0; i < ARRAY_SIZE(bodies); i++) {
		register_body(test, bodies[i]);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		check_state(test, before);
		unregister_first(test);
	}
}

/* A valid later module must never turn an earlier execution failure into success. */
static void categories_and_stop_once(struct kunit *test, const char *body)
{
	static const char second[] =
		"local M={name='dispatch_second'," META "hit=0};"
		"local function f() M.hit=M.hit+1;return true end;" HOOKS "return M\n";
	struct snapshot before = snapshot(test);
	kernel_cap_t e = CAP_EMPTY_SET, i = e, p = e;
	char *buffer = NULL;
	ssize_t remaining = 99;
	int count = 0;
	lua_State *L;

	register_body(test, body);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(second, sizeof(second) - 1), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_vm_enough_memory(NULL, 1), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_task_prctl(0, 0, 0, 0, 0), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_fs_context_parse_param(NULL, NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_xattr_skipcap("x"), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_ismaclabel("x"), 1);
	KUNIT_EXPECT_EQ(test, lua_lsm_audit_rule_known(NULL), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_need_killpriv(NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_audit_rule_match(NULL, 0, 0, NULL), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_capget(current, &e, &i, &p), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_init_security(NULL, NULL, NULL, NULL, &count), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_getsecurity(NULL, NULL, "x", (void **)&buffer, true), -EIO);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_listsecurity(NULL, &buffer, &remaining), -EIO);
	lua_lsm_inode_post_setxattr(NULL, "x", "v", 1, 0);
	KUNIT_EXPECT_PTR_EQ(test, buffer, NULL);
	KUNIT_EXPECT_EQ(test, remaining, (ssize_t)99);
	KUNIT_EXPECT_EQ(test, count, 0);
	L = lvm_get();
	lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
	lua_pushliteral(L, "dispatch_second");
	lua_rawget(L, -2);
	/* The subsequent module was never even lazily loaded. */
	KUNIT_EXPECT_TRUE(test, lua_isnil(L, -1));
	lua_pop(L, 2);
	lvm_put(L);
	check_state(test, before);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("dispatch_second"), 0);
	unregister_first(test);
}

static void categories_and_stop(struct kunit *test)
{
	categories_and_stop_once(test, "error({})");
	categories_and_stop_once(test, "string.byte({})");
}

static void lazy_and_lookup_errors(struct kunit *test)
{
	const char *stages[] = {
		"if current then error('load') end;",
		"if current then string.byte({}) end;",
		"if current then return 42 end;",
		"if current then return setmetatable({}, {__index=function() error({}) end}) end;",
		"if current then return {} end;",
		/* current assignment before argument construction invokes __newindex. */
		"if current then local f=function()return true end;"
		"setfenv(f,setmetatable({}, {__newindex=function()error('env')end}));"
		"return {file_open=f} end;",
	};
	struct snapshot before = snapshot(test);
	int i;

	for (i = 0; i < ARRAY_SIZE(stages); i++) {
		char *code = kunit_kmalloc(test, 4096, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, code);
		snprintf(code, 4096,
			"%s return {name='dispatch_first'," META "file_open=function()return true end}\n", stages[i]);
		KUNIT_ASSERT_NOT_NULL(test, code);
		KUNIT_ASSERT_EQ(test, lua_lsm_module_register(code, strlen(code)), 0);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
		check_state(test, before);
		unregister_first(test);
	}
}

static void environment_and_reuse(struct kunit *test)
{
	static const char cleanup_policy[] =
		"local M={name='dispatch_first'," META "};"
		"M.file_free_security=function(file)file.marker=42;"
		"M.cached=file.marker;error({})end;return M\n";
	struct snapshot before = snapshot(test);
	struct file *file = kunit_kzalloc(test, sizeof(*file), GFP_KERNEL);
	struct lua_lsm_object *object;
	lua_State *L;

	register_body(test, "if M.hit==1 then setfenv(0,{});error({}) end;return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	check_state(test, before);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	unregister_first(test);
	register_body(test, "if M.hit==1 then debug.sethook(function()error({})end,'',1) end;assert(debug.gethook()==nil);return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO);
	check_state(test, before);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	unregister_first(test);
	KUNIT_ASSERT_NOT_NULL(test, file);
	file->f_security = kunit_kzalloc(test, lua_lsm_blob_sizes.lbs_file +
					   sizeof(*object), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, file->f_security);
	object = lua_lsm_file(file);
	KUNIT_ASSERT_EQ(test, lua_lsm_module_register(cleanup_policy,
						   sizeof(cleanup_policy) - 1), 0);
	lua_lsm_file_free_security(file);
	/* __postpone must free a node created just before the Lua error. */
	KUNIT_EXPECT_EQ(test, atomic_read(&object->dict.count), 0);
	L = lvm_get();
	lua_getfield(L, LUA_REGISTRYINDEX, "_MODULES");
	lua_getfield(L, -1, "dispatch_first");
	lua_getfield(L, -1, "cached");
	KUNIT_EXPECT_EQ(test, lua_tointeger(L, -1), (lua_Integer)42);
	lua_pop(L, 3);
	lvm_put(L);
	check_state(test, before);
	unregister_first(test);
}

static void caught_and_valid_results(struct kunit *test)
{
	struct snapshot before = snapshot(test);
	struct vm_area_struct vma = {};

	register_body(test, "local ok=pcall(string.byte,{});assert(not ok);"
		"ok=xpcall(function()error({})end,function(e)return e end);assert(not ok);"
		"local co=coroutine.create(function()error({})end);ok=coroutine.resume(co);"
		"assert(not ok);return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	unregister_first(test);
	register_body(test, "return false,13");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EACCES);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_need_killpriv(NULL), -EACCES);
	unregister_first(test);
	register_body(test, "return nil");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_getsecurity(NULL, NULL, "x", NULL, false), -EOPNOTSUPP);
	unregister_first(test);
	register_body(test, "return 'value'");
	KUNIT_EXPECT_EQ(test, lua_lsm_inode_getsecurity(NULL, NULL, "x", NULL, false), 5);
	unregister_first(test);
	register_body(test, "local v=...;assert(v:start()==0);"
		"assert(debug.getfenv(v).current==current);return true");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_mprotect(&vma, 0, 0), 0);
	unregister_first(test);
	check_state(test, before);
}

struct failing_alloc {
	lua_Alloc original;
	void *ud;
	int remaining;
	bool failed;
};
static void *fail_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct failing_alloc *a = ud;

	if (nsize > osize && (--a->remaining <= 0 || a->failed)) {
		a->failed = true;
		return NULL;
	}
	return a->original(a->ud, ptr, osize, nsize);
}
static void allocation_failures(struct kunit *test)
{
	struct snapshot before = snapshot(test);
	int n, failures = 0;

	register_body(test, "assert(debug.gethook()==nil);debug.sethook(function()end,'',100);"
		"local t={};for i=1,32 do t[i]=tostring(i)..'oom' end;return true");
	for (n = 1; n <= 48; n++) {
		struct failing_alloc a = { .remaining = n };
		lua_State *L = lvm_get();
		int ret;

		a.original = lua_getallocf(L, &a.ud);
		lua_setallocf(L, fail_alloc, &a);
		lvm_put(L);
		ret = lua_lsm_file_open(NULL);
		L = lvm_get();
		lua_setallocf(L, a.original, a.ud);
		lvm_put(L);
		if (a.failed) {
			failures++;
			KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
		} else {
			KUNIT_EXPECT_EQ(test, ret, 0);
		}
		check_state(test, before);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
	}
	KUNIT_EXPECT_GT(test, failures, 8);
	kunit_info(test, "persistent OOM injection: %d/48 positions failed safely\n", failures);
	unregister_first(test);
}

/* Re-register each time so injection reaches fresh loader/parser allocations. */
static void lazy_allocation_failures(struct kunit *test)
{
	struct snapshot before = snapshot(test);
	int n, failures = 0;

	for (n = 1; n <= 96; n++) {
		struct failing_alloc a = { .remaining = n };
		lua_State *L;
		int ret;

		register_body(test, "return true");
		L = lvm_get();
		a.original = lua_getallocf(L, &a.ud);
		lua_setallocf(L, fail_alloc, &a);
		lvm_put(L);
		ret = lua_lsm_file_open(NULL);
		L = lvm_get();
		lua_setallocf(L, a.original, a.ud);
		lvm_put(L);
		if (a.failed) {
			failures++;
			KUNIT_EXPECT_EQ_MSG(test, ret, -ENOMEM, "loader allocation %d", n);
		} else {
			KUNIT_EXPECT_EQ(test, ret, 0);
		}
		check_state(test, before);
		KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
		unregister_first(test);
	}
	KUNIT_EXPECT_GT(test, failures, 32);
	kunit_info(test, "lazy OOM injection: %d/96 positions failed safely\n", failures);
}

struct softirq_check {
	struct tasklet_struct tasklet;
	struct completion done;
	int err, predicate, top;
	bool in_softirq, restored;
};
static void softirq_callback(struct tasklet_struct *t)
{
	struct softirq_check *s = from_tasklet(s, t, tasklet);
	lua_State *L = lvm_get();
	const void *env = lua_topointer(L, LUA_GLOBALSINDEX);

	lvm_put(L);
	s->in_softirq = in_serving_softirq();
	s->err = lua_lsm_file_open(NULL);
	s->predicate = lua_lsm_inode_xattr_skipcap("x");
	L = lvm_get();
	s->top = lua_gettop(L);
	s->restored = env == lua_topointer(L, LUA_GLOBALSINDEX) && !lua_gethook(L);
	lvm_put(L);
	complete(&s->done);
}
static void task_atomic_and_softirq(struct kunit *test)
{
	struct softirq_check s;
	struct snapshot before = snapshot(test);
	int ret;

	register_body(test, "error({})");
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), -EIO); /* Populate task VM. */
	preempt_disable();
	ret = lua_lsm_file_open(NULL);
	preempt_enable();
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	check_state(test, before);
	init_completion(&s.done);
	tasklet_setup(&s.tasklet, softirq_callback);
	tasklet_schedule(&s.tasklet);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s.done, 10 * HZ), 0UL);
	tasklet_kill(&s.tasklet);
	KUNIT_EXPECT_TRUE(test, s.in_softirq);
	KUNIT_EXPECT_EQ(test, s.err, -EIO);
	KUNIT_EXPECT_EQ(test, s.predicate, 0);
	KUNIT_EXPECT_EQ(test, s.top, 0);
	KUNIT_EXPECT_TRUE(test, s.restored);
	unregister_first(test);
}

static struct kunit_case dispatch_cases[] = {
	KUNIT_CASE(errno_errors),
	KUNIT_CASE(categories_and_stop),
	KUNIT_CASE(lazy_and_lookup_errors),
	KUNIT_CASE(environment_and_reuse),
	KUNIT_CASE(caught_and_valid_results),
	KUNIT_CASE(allocation_failures),
	KUNIT_CASE(lazy_allocation_failures),
	KUNIT_CASE(task_atomic_and_softirq),
	{}
};
static struct kunit_suite dispatch_suite = {
	.name = "lua-lsm-dispatch-errors",
	.init = lua_lsm_test_init,
	.test_cases = dispatch_cases,
};
kunit_test_suite(dispatch_suite);
MODULE_LICENSE("GPL");
