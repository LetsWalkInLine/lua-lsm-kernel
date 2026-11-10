/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_LSM_TEST_COMMON_H
#define LUA_LSM_TEST_COMMON_H

#include <kunit/test.h>

/* Maintained by the LSM framework; unlike initialized, this survives init. */
extern int lua_lsm_enabled;

static inline int lua_lsm_test_init(struct kunit *test)
{
	if (!READ_ONCE(lua_lsm_enabled))
		kunit_skip(test, "Lua-LSM is not active");
	return 0;
}

#endif
