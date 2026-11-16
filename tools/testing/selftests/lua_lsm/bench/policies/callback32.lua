-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario: gsub callback policy (32 Lua callbacks per call).
-- The callback input is generated at lazy load, outside timed calls.
local M={name='bench_callback32',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
local input=string.rep('a',32)
M.file_open=function()
 return #string.gsub(input,'.',function(c) return c end)==32
end
return M
