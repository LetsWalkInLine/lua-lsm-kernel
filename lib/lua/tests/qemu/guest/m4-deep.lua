local errno = require('errno')
local deep = string.rep('a*',64)..'['..string.rep('z',65536)..']'
local plain = string.rep('a',65535)
local cap = string.rep('a',17000)
local src = cap..cap
local outsrc = string.rep('a',33000)
local value = string.rep('a',22000)
local tail = string.rep('a',65536)
local function recovered(ok,err)
  assert(not ok and string.find(err,'string work limit exceeded',1,true))
  assert(string.match('ab','a?b')=='ab')
end
return {
 name='m4_deep', author='local development harness',
 description='Finite deep work errors and bulk precharge', license='GPL-2.0', version=1,
 file_open=function(file,cred)
  local path=file:path()
  local ok,err
  if path=='/m4-deep-find' then
   ok,err=pcall(string.find,'',deep)
  elseif path=='/m4-deep-match' then
   ok,err=pcall(string.match,'',deep)
  elseif path=='/m4-deep-gmatch' then
   ok,err=pcall(string.gmatch('',deep))
  elseif path=='/m4-deep-gsub' then
   ok,err=pcall(string.gsub,'',deep,'x')
  elseif path=='/m4-deep-find-uncaught' then
   string.find('',deep); return false,errno.EACCES
  elseif path=='/m4-deep-match-uncaught' then
   string.match('',deep); return false,errno.EACCES
  elseif path=='/m4-deep-gmatch-uncaught' then
   string.gmatch('',deep)(); return false,errno.EACCES
  elseif path=='/m4-deep-gsub-uncaught' then
   string.gsub('',deep,'x'); return false,errno.EACCES
  elseif path=='/m4-deep-plain-compare' then
   ok,err=pcall(string.find,plain,plain,1,true)
  elseif path=='/m4-deep-capture-compare' then
   ok,err=pcall(string.match,src,'^('..cap..')%1')
  elseif path=='/m4-deep-capture-output' then
   ok,err=pcall(string.match,outsrc,'^(a*)$')
  elseif path=='/m4-deep-whole-output' then
   ok,err=pcall(string.gsub,outsrc,'^a*$','%0')
  elseif path=='/m4-deep-value-output' then
   ok,err=pcall(string.gsub,value,'^(a*)$','%1')
  elseif path=='/m4-deep-tail-output' then
   ok,err=pcall(string.gsub,tail,'z','x',0)
  else return true end
  recovered(ok,err)
  return false,errno.EACCES
 end,
}
