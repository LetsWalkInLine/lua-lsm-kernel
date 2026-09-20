# Isolated in-kernel string measurements

This directory implements the corpus measurement protocol in
[../corpus/README.md](../corpus/README.md). It has no entry in the shared tree's
Makefile: ordinary kernel builds do not link or run it. `prepare.py` adds Kbuild
entries only in an explicitly supplied independent source copy. Never prepare
an overlay in a running host kernel tree or deploy the resulting experimental
kernel. A/B/H remove string work enforcement; execute them only in a guest with
a host watchdog. All corpus inputs are finite, including optional patterns up
to n=14. No production Lua setter, getter or limit switch is added.

The inputs to `prepare.py` are an independent kernel source tree, the generated
corpus manifest and an exact post-M2/pre-M3 `lstrlib.c` snapshot. The latter must
be exported with the measurement evidence; its SHA-256 is recorded, not inferred
from an old binary. This implementation expects the inherited gmatch fix which
commits the iterator cursor after successful capture output. The source base and
all uncommitted patches must be exported with the result. Preserve all earlier
builds and use new output directories.

```sh
python3 lib/lua/tests/corpus/generate.py --output /tmp/corpus-v1
python3 lib/lua/tests/benchmark/prepare.py \
  --source /path/to/independent-kernel-source \
  --historical /path/to/exported-post-m2-lstrlib.c \
  --manifest /tmp/corpus-v1/manifest.json
```

The isolated copy needs `CONFIG_LUA=y`, `CONFIG_KUNIT=y` and
`CONFIG_LUA_KUNIT_TEST=y`. Reuse the same baseline configuration/compiler for all
variants. Link all five variants into one kernel so the core and configuration
are identical. The default still runs no benchmark. Boot with `lua_bench=A`,
`B`, `C`, `D` or `H`, plus `lua_bench_boot=0` through `4`. A trial run can add
`lua_bench_pilot`. Add `kunit.enable=0` for timing runs after the appropriate
functional regression has passed. The host must require `M5BENCH END PASS`, a
clean console and the existing independent smoke result; a benchmark failure
returns from init and never becomes an implicit success.

`prepare.py` makes private A/B/D copies from the current ordinary library. A
compiles begin/charge/end to no-ops but retains current scanning helpers. B has
no debit or enforcement but records every logical charge. D observes the
ordinary finite quota. C opens the unmodified ordinary string library. H uses
the supplied historical implementation plus the same cursor fix, preserving
post-M2 depth protection and original scanning helpers. H/A can also differ in
historical state layout and optimized control flow; report those object/stack
facts instead of attributing their entire difference to one helper instruction.
The ordinary production `.text` must be checked against a standalone build of
the same inherited production source.

The bounded plain scan batches its logical byte charge after `memchr`, whose
length is limited by the remaining allowance before reading. A/B also remove
this scan allowance; otherwise A would read an uninitialized budget and B
could not observe complete demand beyond the production limit. D retains it.
Successful scan totals and the one-byte rejection at exhaustion are unchanged.

Generated chunks end with a newline, as the corpus verifiers do. This avoids
the current lexer's EOF edge when an identifier/keyword ends at the final byte;
this harness does not repair the lexer.

The benchmark runs in the init task via a late initcall, in task context with
normal preemption and interrupts. It is a private C-created Lua state, not a
real file_open call or the Lua-LSM traceback handler path. Error durations
include protected Lua error construction/unwind, without the LSM diagnostic
printing. They cannot be substituted for full authorization-path rejection
latency. Timer calls surround `lua_pcall` only; input generation, Lua stack
preparation, validation, reuse checks and printing are outside the interval.
The additional EMPTY closure establishes the protected-call/timer floor. A
two-second delay precedes measurement to allow this environment's boot TSC
refinement; verify the clocksource transition timestamps for every run rather
than assuming the delay universally guarantees stabilization.

Timing variants A/C/H use 20 fresh-state blocks per successful sample per boot.
Each block has a full GC before five warmups followed by 100 recorded calls:
100 warmups and 2,000 samples per boot in total. Expected errors use one block,
five warmups and 50 calls. A pilot uses two warmups and ten calls. Result values
and types are checked after every call, not just during the pilot. String
comparisons used for validation can warm data caches, so these are explicitly
warm, reused-input measurements. Normal GC remains enabled. The iterator reset
closure recreates consumed iterator state outside every timed sequence; its
allocation may still affect subsequent normal GC. Case order rotates by seven
positions per boot, with no random seed or hidden filtering.

B/D instead perform two deterministic observed calls per sample, including
terminal iterator calls and separate nested callback operations. Counters and
depth high-water values live in heap fixtures and survive longjmp. A record
holds admitted depth after the work/depth guards and attempted depth before
either guard. Plain calls have depth zero. Counts are requested logical charge,
not instructions or exact touched bytes. B's record-internal remaining value is
not meaningful because debit is disabled; export it as null.

Parent IDs use the Lua call-frame ordinal and unfinished earlier records. This
is sufficient for the current corpus: its nested callbacks complete normally,
and errors leave the top-level protected call before the fixture is reset. It
is not general tracking of arbitrary caught callback errors that unwind and
later reenter at reused frame slots. Such a corpus extension requires real
unwind-aware tracking or separate explicit calls before its measurements are
accepted. Full/overflow flags invalidate a run; the observer never silently
omits records. Fixtures and Lua states are freed on both successful and failed
runs.

Each console `M5T` record contains a complete block of raw integer nanosecond
samples; `M5O` declares an observed invocation and record count; `M5W` contains
its operation counters, parent ID and depth maxima. Export parsers, console
logs, argv, environment, generation.json, source/config/kernel hashes and
per-call JSONL together. A parser must enforce sample/block/repeat completeness,
unique keys, outcomes, counter sums, exact boundary formulas and observer flags
before reporting statistics. Store timeout/failure runs with their reason. Do
not report a production distribution from equally weighted synthetic cases or
p99 from fewer than 1,000 calls. Keep diagnostic/sanitizer data separate from
latency data. Default-value changes still require the workload/compatibility
judgment described by the corpus protocol.
