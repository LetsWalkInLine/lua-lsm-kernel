-- SPDX-License-Identifier: GPL-2.0-only
local M={name='strings',author='Lua-LSM tests',license='GPL-2.0',version=1,
 description='String errors remain catchable under the VM deadline'}
local function excess()
 -- Empty output still charges repetitions; no large lazy-load allocation.
 return string.rep('',1600000)
end
M.file_open=function(file)
 local p=file:path()
 if p=='/strings-ok' then
  assert(string.sub('abcdef',2,4)=='bcd')
  assert(string.gsub('abc','b','B')=='aBc')
 elseif p=='/strings-caught' then
  local ok,err=pcall(excess)
  assert(not ok and string.find(err,'string work limit exceeded',1,true))
  assert(string.match('ab','a?b')=='ab')
 elseif p=='/strings-uncaught' then excess()
 else return true end
 return true
end
return M
