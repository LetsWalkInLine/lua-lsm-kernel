local M = {name='budget_errors',author='Lua-LSM tests',
 description='Fatal timeouts and recovery',license='GPL-2.0',version=1}
M.file_open=function(file)
 local path=file:path()
 if path=='/budget-loop' then while true do end
 elseif path=='/budget-pcall' then
  pcall(function()while true do end end);return true
 elseif path=='/budget-xpcall' then
  while true do xpcall(function()error({})end,function(e)return e end)end
 elseif path=='/budget-handler' then
  xpcall(function()error('x')end,function()while true do end end);return true
 elseif path=='/budget-environment' then setfenv(0,{});while true do end
 elseif path=='/budget-hook' then
  debug.sethook(function()while true do end end,'',1);return true
 elseif path=='/budget-coroutine' then
  local co=coroutine.create(function()while true do end end)
  coroutine.resume(co);return true
 elseif path=='/budget-callback' then
  string.gsub('x','.',function()pcall(function()while true do end end);return ''end)
  return true
 elseif path=='/budget-return' then
  local armed=false
  debug.sethook(function(e)
   if armed then while true do end
   elseif debug.getinfo(2,'f').func==M.file_open then armed=true end
  end,'r')
  return true
 elseif path=='/budget-caught' then
  assert(not pcall(error,{}))
  assert(not xpcall(function()error('normal')end,function(e)return e end))
  assert(not coroutine.resume(coroutine.create(function()error('normal')end)))
  return true
 elseif path=='/budget-deny' then return false,13
 elseif path=='/budget-ok' then assert(debug.gethook()==nil);return true
 end
end
return M
