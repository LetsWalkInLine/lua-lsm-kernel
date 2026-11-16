-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario (budget trees only): pure-Lua loop until the
-- per-module VM deadline; every openat returns -EIO.
local M={name='bench_timeout',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
M.file_open=function() while true do end end
return M
