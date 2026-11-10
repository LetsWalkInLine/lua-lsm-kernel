local errno = require("errno")

return {
  name = "smoke",
  author = "Lua-LSM tests",
  description = "Deny reads of the Lua-LSM smoke-test target",
  license = "GPL-2.0",
  version = 1,

  file_open = function(file, cred)
    local path = file:path()
    if path == "/lua-lsm-denied" then
      return false, errno.EACCES
    end
    return true
  end,
}
