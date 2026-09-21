local errno=require('errno')
-- 262 + 3*21758 = 65536 units before SW_ENTER in the 66th frame.
local source=string.rep('b',21758)
local pattern=source..string.rep('a*',65)
return {
 name='m4_enter66',author='local development harness',license='GPL-2.0',version=1,
 description='Work rejection before the 66th-frame depth check',
 file_open=function(file,cred)
  local path=file:path()
  if path=='/m4-enter66-caught' then
   local ok,err=pcall(string.gsub,source,pattern,'x')
   assert(not ok and string.find(err,'string work limit exceeded',1,true))
   assert(string.match('ab','a?b')=='ab')
  elseif path=='/m4-enter66-uncaught' then
   string.gsub(source,pattern,'x')
  else return true end
  return false,errno.EACCES
 end,
}
