# String operation limits

The kernel string library limits pattern recursion and selected work within
each operation. These limits protect library calls; they do not provide a
VM-wide execution budget or make Lua-LSM hooks fail closed. An uncaught Lua
error can leave a hook's default allow result in effect.

| Operation | Work budget lifetime |
| --- | --- |
| `string.find`, including plain search | One call |
| `string.match` | One call |
| `string.gsub` | One call, including all replacements |
| `string.gmatch` / `string.gfind` | One iterator invocation |

Each operation starts with 65,536 internal work units. Pattern matching also
permits 65 active matcher calls, including the root, to preserve 32 ordinary
captures. These values are not Lua-configurable or a public ABI.

The budget charges matcher steps, searches, scans, retries, comparisons,
replacements and direct output. Long comparisons and output requests are charged
by requested length before the action; plain first-byte search scans at most
the current allowance. An affordable charge that reaches zero succeeds. The
next positive charge raises `string work limit exceeded`, without truncating
input or output. Exceeding matcher depth raises `pattern recursion limit
exceeded`. Both are ordinary Lua errors; whichever check is reached first
determines the diagnostic.

`pcall` and `xpcall` may catch these errors, and later calls receive fresh
budgets. Nested string calls made by `gsub` callbacks each receive their own
budget. A `gmatch` iterator commits its cursor only after it constructs the
result, so a failed invocation does not skip a match. Repeatedly catching and
retrying an unaffordable call can still run indefinitely.

Policies that need denial on a string error must handle it explicitly:

```lua
local errno = require('errno')
local ok, matched = pcall(string.match, file:path(), pattern)
if not ok then
    return false, errno.EACCES
end
-- Continue the policy decision using matched.
```

The values were checked against finite policy-shaped, boundary and pathological
samples on x86_64. A 4095-byte path sample required 24,568 units; a 32-capture
sample reached the full depth of 65. These samples do not establish production
latency or rejection rates.

The budget does not cover pure Lua loops, the full iterator traversal, all C
libraries, callback internals, allocation, garbage collection or every buffer
operation. The depth guard does not bound total C stack under nested callbacks
or arbitrary LSM call chains. x86_64 tests cover selected configurations and
error recovery, not every architecture or atomic/softirq context. The selected
sanitizer run omitted `UBSAN_BOUNDS` because of a pre-existing Lua closure
layout issue; three instrumented `gsub` frame warnings remain. These limits do
not change the existing hook error handling or automatically unload policies.
