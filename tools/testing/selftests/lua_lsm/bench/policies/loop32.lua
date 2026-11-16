-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario: small arithmetic loop policy.
local M={name='bench_loop32',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
M.file_open=function()
 local x=0
 for i=1,32 do x=x+i end
 assert(x==528)
 return true
end
return M
