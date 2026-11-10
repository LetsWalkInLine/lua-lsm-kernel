local M = {name='dispatch_errors',author='Lua-LSM tests',
 description='Uncaught error propagation and same-task recovery',license='GPL-2.0',version=1}
M.file_open=function(file)
 local path=file:path()
 if path=='/dispatch-ordinary' then error('ordinary')
 elseif path=='/dispatch-table' then error({})
 elseif path=='/dispatch-number' then error(42)
 elseif path=='/dispatch-library' then string.byte({})
 elseif path=='/dispatch-environment' then setfenv(0,{});error({})
 elseif path=='/dispatch-hook' then debug.sethook(function()error({})end,'',1)
 elseif path=='/dispatch-coroutine' then
  local co=coroutine.create(function()error({})end)
  local ok,err=coroutine.resume(co);assert(not ok);error(err)
 elseif path=='/dispatch-caught' then
  local ok=pcall(string.byte,{});assert(not ok)
  ok=xpcall(function()error({})end,function(e)return e end);assert(not ok)
  return true
 elseif path=='/dispatch-deny' then return false,13
 elseif path=='/dispatch-ok' then assert(debug.gethook()==nil);return true
 end
 return nil
end
return M
