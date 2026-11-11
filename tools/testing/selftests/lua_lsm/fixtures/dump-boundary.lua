-- SPDX-License-Identifier: GPL-2.0-only
return {name="dump_boundary",license="GPL",file_open=function(file)
 local path=file:path()
 if path=="/dump32-source" then
  local f=assert(loadstring(blobs[32]))
  local copy=assert(loadstring(string.dump(f)))
  for i=1,31 do copy=copy() end
  assert(copy()==42)
 elseif path=="/dump33-caught" then
  local f=assert(loadstring(blobs[33]))
  local ok,err=pcall(string.dump,f)
  assert(not ok and string.find(err,"unable to dump",1,true))
  assert(loadstring(string.dump(function()return 42 end))()==42)
 else return true end
 return false,errno.EACCES
end}
