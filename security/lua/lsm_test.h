/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUA_LSM_TEST_H
#define LUA_LSM_TEST_H

#include <linux/lualib.h>
#include <linux/slab.h>

/* Stable resource/lifecycle boundaries, redirected only by the current KUnit. */
#ifdef CONFIG_SECURITY_LUA_LSM_KUNIT_TEST
void *lua_lsm_security_memdup(const void *src, size_t size, gfp_t flags);
void lua_lsm_security_free(const void *ptr);
void lua_lsm_module_resetdebughook(lua_State *L);
#else
#define lua_lsm_security_memdup kmemdup
#define lua_lsm_security_free kfree
#define lua_lsm_module_resetdebughook lua_resetdebughook
#endif

#endif
