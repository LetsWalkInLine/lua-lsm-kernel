# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makefile for the Lua based LSM
#

obj-$(CONFIG_SECURITY_LUA_LSM) := lua-lsm.o

ccflags-$(CONFIG_SECURITY_LUA_LSM_DEBUG) += -DDEBUG

lua-lsm-y := lsm.o securityfs.o auxlib.o kvcache.o lsm_defs.o \
	lua_mm.o \
	lua_kernel.o lua_fs.o lua_net.o \
	lua_errno.o lua_capability.o lua_signal.o lua_audit.o

lua-lsm-$(CONFIG_SECURITY_LUA_LSM_KUNIT_TEST) += dispatch_error_kunit.o
lua-lsm-$(CONFIG_SECURITY_LUA_LSM_KUNIT_TEST) += vm_budget_kunit.o vm_budget_clock_kunit.o
lua-lsm-$(CONFIG_SECURITY_LUA_LSM_KUNIT_TEST) += vm_budget_review_kunit.o vm_getsecurity_review_kunit.o
lua-lsm-$(CONFIG_SECURITY_LUA_LSM_KUNIT_TEST) += vm_budget_combined_kunit.o

lua-lsm-$(CONFIG_SECURITY_LUA_LSM_TEST_PROBE) += tests/receive_probe.o
