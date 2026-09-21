# String operation limits

The kernel string library limits active pattern recursion and selected work
inside each operation. These are library robustness limits, **not a complete
Lua-LSM authorization boundary**. An uncaught Lua error can still leave a hook's
default allow result in effect. There is no VM-wide execution budget or automatic
fail-close (denying access when policy execution fails).

## Scope and compatibility

| Operation | Budget lifetime |
| --- | --- |
| `string.find`, including plain search and automatic plain classification | One call |
| `string.match` | One call |
| `string.gsub` | One call, including all replacement rounds |
| Iterator returned by `string.gmatch` / `string.gfind` | Each iterator invocation, not the whole traversal |

Each operation starts with an internal allowance of 65,536 work units. Pattern
matching independently allows 65 simultaneously active matcher calls, including
the root call. This preserves 32 ordinary captures; other recursive constructs
in the same pattern can still exceed the limit. Neither constant is a public
ABI or a Lua-configurable setting.

Control events cost one unit: operation entry, a new subject start, matcher
entry/dispatch, retry, capture-slot checks, plain candidates, capture emission,
replacement rounds and callback/table dispatch. Scanning costs logical bytes.
Comparisons cost their requested length, even if `memcmp` could stop early.
Direct output requests cost their requested bytes. Comparisons and output are
charged before the protected action. Plain first-byte search clamps `memchr` to
the remaining allowance and charges the actual scanned prefix. These internal
units are not microseconds or a stable performance interface.

An affordable request that leaves zero units succeeds. The next positive charge
fails. An unaffordable request is rejected without unsigned wraparound; it does
not silently shorten the input, pattern or result. Automatic plain classification
retains its existing first-NUL stopping rule; explicit plain comparisons use the
full Lua string lengths.

## Errors, recovery and iterators

The diagnostics are ordinary Lua errors:

- `pattern recursion limit exceeded`
- `string work limit exceeded`

The first check reached determines the error; work charged at matcher entry can
fail before the depth check. `pcall` / `xpcall` can catch either error and the
same Lua state can be reused. Catching an error does not create a VM-wide sticky
exhaustion flag. Error text is for diagnosis; dispatch code must not parse it to
classify a security decision.

A `gmatch` invocation commits its iterator cursor only after successfully
constructing the returned captures and securing the required Lua stack slot.
A budget error does not skip the failed match. Repeating the same call with the
same insufficient allowance keeps failing; an unlimited `pcall` retry loop can
therefore run indefinitely. Successful zero-width matches retain their normal
cursor advancement. Creating an iterator does not prevalidate all its work.

A `gsub` replacement function or table metamethod can call string operations
again. Each nested operation gets its own allowance; the outer allowance is
neither reset nor combined with it. Existing Lua protected-call cleanup restores
interpreter state, but side effects of callbacks are not rolled back and buffer
memory need not be collected immediately.

For a policy that chooses to deny on any matching error, explicitly return a
deny decision from the hook rather than relying on an uncaught error:

```lua
local errno = require('errno')
-- Within file_open(file, cred), with pattern supplied by the policy:
local ok, matched = pcall(string.match, file:path(), pattern)
if not ok then
    return false, errno.EACCES
end
-- Continue the policy's normal decision using matched.
```

This example handles the protected call only. It does not make the whole hook
fail-close, nor prevent errors elsewhere in the policy.

## Why these internal values remain candidates

The versioned corpus and actual x86_64 kernel measurements support keeping
65 / 65,536 as internal values. Twenty ordinary policy-shaped samples required
at most 123 units per operation. The normal boundary sample BPATH4095 required
24,568 units (37.49% of the allowance), leaving 40,968. BCAP32 required 324
units but reached depth 65, leaving **no depth margin**. The pathological AOPT14
required 147,452 units without enforcement and was rejected with enforcement.
These are finite synthetic and repository-derived samples, not a production
workload distribution.

The paired ordinary-library measurements reduced long plain-search mean time
from 146.623 to 35.925 microseconds. The BPATH4095 pattern mean instead changed
from 75.457 to 84.181 microseconds (+11.6%); the cause remains unresolved.
Production traffic, acceptable latency and normal-input rejection rates are
unknown. Keeping the implementation does not establish production performance
acceptance. See the [validation and evidence index](../../../lib/lua/tests/VALIDATION.md)
for environments, full distributions, failures and scope.

## What remains unbounded or unverified

- Repeated calls, the whole iterator traversal, callback/table internals, pure
  Lua loops and other C libraries have no shared execution budget.
- `string.rep` and other uncovered string entry points remain outside this limit.
  Buffer merging, string interning, GC and conversions are not fully charged.
  Thus even a single covered API has no proven wall-clock upper bound.
- Nested replacements and error handlers can accumulate C stack frames beyond
  the local matcher guard. The measurements do not prove an arbitrary kernel
  call-chain stack bound, atomic/softirq safety or other-architecture safety.
- The selected sanitizer run disabled `UBSAN_BOUNDS` because the old closure
  `upvalue[1]` tail declaration conflicts with bounds-strict instrumentation.
  That combination failed and remains a coverage gap. Modernizing the closure
  layout is separate work.
- The KASAN longjmp cleanup fix and its validation cover x86_64. Three known
  instrumented `gsub` compiler frame warnings remain documented. A 32 KiB KASAN
  task stack cannot be compared directly with the old 16 KiB configuration.
- Full VM limits, fail-close dispatch and production automatic isolation are
  separate work. This change neither skips nor automatically unloads policies.
