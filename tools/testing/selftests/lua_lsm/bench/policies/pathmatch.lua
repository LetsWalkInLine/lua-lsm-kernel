-- SPDX-License-Identifier: GPL-2.0-only
-- Benchmark scenario: README-style path pattern match on every open.
local M={name='bench_pathmatch',author='Lua-LSM benchmark',
 description='openat bench',license='GPL-2.0',version=1}
M.file_open=function(file)
 local path=file:path()
 if path:match('^/etc/') then return false,13 end
 return true
end
return M
