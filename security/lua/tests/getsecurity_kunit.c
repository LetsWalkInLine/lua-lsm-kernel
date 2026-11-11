// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the real dispatcher; KUnit-scoped stubs control resource boundaries. */
#include <kunit/test.h>
#include <kunit/static_stub.h>
#include "test_common.h"
#include <linux/lauxlib.h>
#include <linux/lualib.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include "../lsm.h"
#include "../lsm_defs.h"
#include "../../../lib/lua/lstate.h"

static lua_State *test_vm;
static void *allocation;
static unsigned int allocations, frees;
static int inject; /* 0 normal, 1 allocation timeout, 2 OOM, 3/4 C completion */
static bool completion_seen, completion_active;

static void completion_hook(lua_State *L, lua_Debug *ar)
{
	completion_seen = true;
	completion_active = G(L)->budget_active;
	lua_sethook(L, NULL, 0, 0);
	if (inject == 3)
		G(L)->deadline = ktime_get_ns() - 1;
	else if (inject == 4)
		lua_gc(L, LUA_GCCOLLECT,
		       0); /* Deferred Lua finalizer, inside C return. */
}

static void stub_resetdebughook(lua_State *L)
{
	lua_resetdebughook(L);
	if (inject == 3 || inject == 4)
		lua_sethook(L, completion_hook, LUA_MASKRET, 0);
}

static void *stub_memdup(const void *src, size_t size, gfp_t flags)
{
	if (inject == 2)
		return NULL;
	allocation = kmemdup(src, size, flags);
	if (allocation)
		allocations++;
	if (inject == 1)
		G(test_vm)->deadline = ktime_get_ns() - 1;
	return allocation;
}

static void stub_free(const void *ptr)
{
	if (ptr && ptr == allocation) {
		frees++;
		allocation = NULL;
	}
	kfree(ptr);
}

static void getsecurity_completion_owns_output(struct kunit *test)
{
	static const char policy[] =
		"return {name='getsecurity_review',author='tests',"
		"description='review',license='GPL-2.0',version=1,"
		"inode_getsecurity=function(idmap,inode,name)"
		"if name=='gc' then collectgarbage('stop');local p=newproxy(true);"
		"getmetatable(p).__gc=function()while true do end end end;"
		"return 'value' end}\n";
	void *buffer, *sentinel = &buffer;
	int ret, i;

	KUNIT_ASSERT_EQ(test,
			lua_lsm_module_register(policy, sizeof(policy) - 1), 0);
	test_vm = lvm_get();
	KUNIT_ASSERT_NOT_NULL(test, test_vm);
	lvm_put(test_vm);
	kunit_activate_static_stub(test, lua_lsm_security_memdup, stub_memdup);
	kunit_activate_static_stub(test, lua_lsm_security_free, stub_free);
	kunit_activate_static_stub(test, lua_lsm_module_resetdebughook,
				   stub_resetdebughook);
	/* Repeated completed allocations must be freed if completion becomes EIO. */
	for (i = 0; i < 8; i++) {
		allocations = frees = 0;
		inject = 1;
		buffer = NULL;
		ret = lua_lsm_inode_getsecurity(NULL, NULL, "x", &buffer, true);
		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test, allocations, 1U);
		KUNIT_EXPECT_EQ(test, frees, 1U);
		KUNIT_EXPECT_PTR_EQ(test, buffer, NULL);
		if (allocation) /* Clean up even on the intentionally broken baseline. */
			stub_free(allocation);
		/* A failed conversion must not overwrite or free the caller's storage. */
		inject = 2;
		buffer = sentinel;
		KUNIT_EXPECT_EQ(test,
				lua_lsm_inode_getsecurity(NULL, NULL, "x",
							  &buffer, true),
				-ENOMEM);
		KUNIT_EXPECT_PTR_EQ(test, buffer, sentinel);
		/* alloc=false accepts a NULL output and never allocates. */
		inject = 0;
		allocations = frees = 0;
		KUNIT_EXPECT_EQ(test,
				lua_lsm_inode_getsecurity(NULL, NULL, "x", NULL,
							  false),
				5);
		KUNIT_EXPECT_EQ(test, allocations, 0U);
		buffer = NULL;
		KUNIT_EXPECT_EQ(test,
				lua_lsm_inode_getsecurity(NULL, NULL, "x",
							  &buffer, true),
				5);
		KUNIT_EXPECT_NOT_NULL(test, buffer);
		KUNIT_EXPECT_EQ(test, allocations, 1U);
		KUNIT_EXPECT_EQ(test, frees, 0U);
		if (buffer) {
			KUNIT_EXPECT_EQ(test, memcmp(buffer, "value", 5), 0);
			stub_free(buffer);
		}
		KUNIT_EXPECT_EQ(test, frees, 1U);
	}
	/* Expiry after C poscall must still fail and free the staged output. */
	completion_seen = completion_active = false;
	allocations = frees = 0;
	inject = 3;
	buffer = NULL;
	KUNIT_EXPECT_EQ(
		test, lua_lsm_inode_getsecurity(NULL, NULL, "x", &buffer, true),
		-EIO);
	KUNIT_EXPECT_TRUE(test, completion_seen);
	KUNIT_EXPECT_TRUE(test, completion_active);
	KUNIT_EXPECT_PTR_EQ(test, buffer, NULL);
	KUNIT_EXPECT_EQ(test, allocations, 1U);
	KUNIT_EXPECT_EQ(test, frees, 1U);
	if (allocation)
		stub_free(allocation);
	/* A Lua finalizer deferred until C completion must share the same deadline. */
	completion_seen = completion_active = false;
	allocations = frees = 0;
	inject = 4;
	buffer = NULL;
	KUNIT_EXPECT_EQ(test,
			lua_lsm_inode_getsecurity(NULL, NULL, "gc", &buffer,
						  true),
			-EIO);
	KUNIT_EXPECT_TRUE(test, completion_seen);
	KUNIT_EXPECT_TRUE(test, completion_active);
	KUNIT_EXPECT_PTR_EQ(test, buffer, NULL);
	KUNIT_EXPECT_EQ(test, allocations, 1U);
	KUNIT_EXPECT_EQ(test, frees, 1U);
	if (allocation)
		stub_free(allocation);
	kunit_deactivate_static_stub(test, lua_lsm_module_resetdebughook);
	kunit_deactivate_static_stub(test, lua_lsm_security_free);
	kunit_deactivate_static_stub(test, lua_lsm_security_memdup);
	/* A stale failure request cannot affect the unredirected real path. */
	inject = 2;
	allocations = frees = 0;
	buffer = NULL;
	KUNIT_EXPECT_EQ(
		test, lua_lsm_inode_getsecurity(NULL, NULL, "x", &buffer, true),
		5);
	KUNIT_EXPECT_EQ(test, allocations, 0U);
	KUNIT_EXPECT_EQ(test, frees, 0U);
	inject = 0;
	if (buffer) {
		KUNIT_EXPECT_EQ(test, memcmp(buffer, "value", 5), 0);
		kfree(buffer);
	}
	KUNIT_ASSERT_EQ(test, lua_lsm_module_unregister("getsecurity_review"),
			0);
	KUNIT_EXPECT_EQ(test, lua_lsm_file_open(NULL), 0);
}

static struct kunit_case cases[] = {
	KUNIT_CASE(getsecurity_completion_owns_output),
	{}
};
static struct kunit_suite suite = {
	.name = "lua_lsm_getsecurity",
	.init = lua_lsm_test_init,
	.test_cases = cases,
};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
