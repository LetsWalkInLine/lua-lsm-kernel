# Pattern recursion limit

The kernel string matcher permits 65 simultaneously active matcher calls,
including the root. This preserves 32 ordinary captures. Combining captures
with other recursive constructs can still reach the limit.

The guard applies to pattern matching in `string.find`, `string.match`,
`string.gsub` and each `string.gmatch` / `string.gfind` iterator invocation.
Plain search does not enter the matcher. A rejected recursive entry raises
`pattern recursion limit exceeded`, an ordinary Lua error that `pcall` can
catch. Normal returns restore the depth allowance; the next operation starts
with a fresh allowance after an error.

The value is an internal constant, not a Lua-configurable interface. The
x86_64 depth tests cover rejection, backtracking and state reuse. This local
guard does not limit total work or the stack accumulated by nested callbacks
and error handlers. It does not establish a bound for arbitrary kernel call
chains or other architectures.

An uncaught Lua error can leave an LSM hook's default allow result unchanged.
The recursion guard does not provide fail-close authorization behavior or a
VM-wide execution budget.
