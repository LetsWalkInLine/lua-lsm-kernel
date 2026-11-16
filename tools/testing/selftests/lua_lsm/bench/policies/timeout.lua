-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario (budget trees only): pure-Lua loop until the
-- per-module VM deadline, keyed to the benchmark target so guest
-- utilities can still execute; every openat of the target returns
-- -EIO after the deadline.
local M={name='bench_timeout',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
M.file_open=function(file)
 if file:path()=='/bench-target' then while true do end end
 return true
end
return M
