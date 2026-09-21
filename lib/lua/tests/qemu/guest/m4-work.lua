local errno = require('errno')
local source = string.rep('a', 14)
local pattern = string.rep('a?', 14) .. 'b'
local large = string.rep('a', 65536)

-- Only this test policy inspects diagnostics; the LSM dispatcher does not.
local function recovered(ok, err)
  return not ok and
    string.find(err, 'string work limit exceeded', 1, true) and
    string.match('ab', 'a?b') == 'ab'
end

return {
  name = 'm4_work',
  author = 'local development harness',
  description = 'Finite work limit checks in file_open',
  license = 'GPL-2.0',
  version = 1,
  file_open = function(file, cred)
    local path = file:path()
    local ok, err
    if path == '/m4-work-plain' then
      ok, err = pcall(string.find, large, 'z', 1, true)
    elseif path == '/m4-work-find' then
      ok, err = pcall(string.find, source, '^' .. pattern)
    elseif path == '/m4-work-match' then
      ok, err = pcall(string.match, source, '^' .. pattern)
    elseif path == '/m4-work-gmatch' then
      local it = string.gmatch(source, pattern)
      ok, err = pcall(it)
    elseif path == '/m4-work-gsub' then
      ok, err = pcall(string.gsub, source, '^' .. pattern, 'x')
    elseif path == '/m4-work-output' then
      ok, err = pcall(string.gsub, large, 'z', 'x', 0)
    elseif path == '/m4-work-uncaught' then
      string.gsub(source, '^' .. pattern, 'x')
      return false, errno.EACCES
    else
      return true
    end
    assert(recovered(ok, err))
    return false, errno.EACCES
  end,
}
