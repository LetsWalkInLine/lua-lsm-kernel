local errno = require('errno')
local p64 = string.rep('a*', 64)
local p65 = string.rep('a*', 65)
local cap32 = string.rep('(a)', 32)
local src32 = string.rep('a', 32)

local function recovered(ok, err)
  return not ok and
    string.find(err, 'pattern recursion limit exceeded', 1, true) and
    string.match('ab', 'a?b') == 'ab'
end

return {
  name = 'm2_depth',
  author = 'local development harness',
  description = 'Finite matcher depth checks in file_open',
  license = 'GPL-2.0',
  version = 1,
  file_open = function(file, cred)
    local path = file:path()
    if path == '/m2-depth-find' then
      local first, last = string.find('', p64)
      assert(first == 1 and last == 0)
    elseif path == '/m2-depth-match' then
      assert(string.match('', p64) == '')
    elseif path == '/m2-depth-gmatch' then
      local it = string.gmatch('', p64)
      assert(it() == '')
    elseif path == '/m2-depth-gsub' then
      local result, n = string.gsub('', p64, 'X')
      assert(result == 'X' and n == 1)
    elseif path == '/m2-depth-capture32' then
      local values = {string.match(src32, cap32)}
      assert(#values == 32 and values[1] == 'a' and values[32] == 'a')
    elseif path == '/m2-depth-find-error' then
      local ok, err = pcall(string.find, '', p65)
      assert(recovered(ok, err))
    elseif path == '/m2-depth-match-error' then
      local ok, err = pcall(string.match, '', p65)
      assert(recovered(ok, err))
    elseif path == '/m2-depth-gmatch-error' then
      local it = string.gmatch('', p65)
      local ok, err = pcall(it)
      assert(recovered(ok, err))
    elseif path == '/m2-depth-gsub-error' then
      local ok, err = pcall(string.gsub, '', p65, 'X')
      assert(recovered(ok, err))
    elseif path == '/m2-depth-uncaught' then
      string.gsub('', p65, 'X')
      return false, errno.EACCES
    else
      return true
    end
    return false, errno.EACCES
  end,
}
