#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Explicit experiment; requires CONFIG_SECURITY_LUA_LSM_BENCHMARK=y.
exec "$(dirname "$0")/run-qemu.py" --mode benchmark "$@"
