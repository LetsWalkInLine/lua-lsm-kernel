-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario: minimal allow policy.
local M={name='bench_allow',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
M.file_open=function() return true end
return M
