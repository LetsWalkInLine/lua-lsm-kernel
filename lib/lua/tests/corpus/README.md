# Lua-LSM string corpus v1

This corpus prepares M5 measurements against source base
`94d655819af226ae8bb96418bfd1eece827d778c`. It does not select new defaults.
The current internal limits remain depth 65 and 65,536 work units per operation.

The generator, expected results and this protocol belong with the existing
`lib/lua/tests/` sources. They are standalone test material, not part of Kbuild
and not a new Lua API. Keep these files together in a future test/documentation
patch. Generated manifests, logs and local QEMU adapters belong in a fresh
output directory. `.dev/` and `.build/` remain ignored; their presence in one
workspace is not distribution or Git versioning. Before sharing a benchmark,
export its exact manifest, checksums, adapter, configuration and raw data beside
the review patch. The final home of the existing end-to-end scripts still needs
maintainer agreement; this corpus does not move them or change ignore rules.

## Reproduce the inputs

From the repository root, with Python 3 and Lua 5.1.5:

```sh
python3 lib/lua/tests/corpus/generate.py --output /tmp/lua-lsm-corpus-v1
cd /tmp/lua-lsm-corpus-v1
sha256sum -c SHA256SUMS
timeout 15s lua5.1 host.lua
```

The output directory must not exist. Generation does not execute Lua or launch
QEMU. `manifest.json` contains all 44 samples, exact UTF-8/binary bytes, byte
lengths, provenance, assumptions, separate setup and operation source, coverage,
exact semantic return tuple, kernel error expectation and host eligibility.
JSON `null` means a returned Lua nil; an empty tuple means zero returned values.
`expected` describes successful semantics without the new resource guards;
`kernel_error`, when present, takes precedence in a guarded kernel. For
AMALFORMED only the error applies; its empty tuple is not a successful outcome.
`inventory.md` provides a compact sample list. Each `ID.lua` returns a verifier
closure. Invoke it once after loading the chunk. The closure checks protected
call success/error, return count and values, then checks a normal operation on
the same state. Iterator setup creates fresh state for each verifier instance.

The generator emits large repeated strings using `string.rep` during setup.
Input allocation, compilation and assertions are not benchmark operations.
The generated source and its manifest are deterministic. Version identity is
`schema_version` plus the generator and output SHA-256 hashes, not the version
number alone. Change sample IDs/meaning only with a documented corpus revision;
retain the old manifest when comparing revisions. No random input is used.

`host.lua` runs only 35 finite, small cases with stock Lua. It is a semantic
cross-check, not proof of kernel behavior, depth, work or time. The other nine
cases are restricted to an externally supervised guest, including resource
boundaries and the original-error case. All inputs are finite. Do not run
pathological amplification in the host kernel. No infinite input is provided.

For kernel validation, adapt each verifier to an isolated Lua state or one
`file_open` policy. In the latter, invoke it only for a dedicated target and
explicitly deny the target after assertions succeed. An assertion failure must
fail the host harness (the current uncaught Lua error can otherwise allow the
open). Require registration, expected denial, unregistration and restored access
for every sample. Retain console logs and a host timeout of 180 seconds. Keep
KUnit and the existing smoke test as separate checks. Error text is used by
these tests only, never by LSM authorization logic.

## Coverage and missing workload evidence

| IDs | Purpose and provenance |
| --- | --- |
| P01-P03, S01 | Pattern/decision extracted from the repository API example, with synthetic hit, near miss and miss inputs. No captured traffic. |
| P04-P06, F01-F04 | Synthetic prefix/plain search, filename extension, dotfile and UTF-8 byte examples. Explicit/automatic plain paths are separate. |
| X01-X04 | Synthetic xattr names, structured values, rejection and embedded NUL. No filesystem xattr I/O. |
| S02-S06 | Synthetic slash rewrite, table replacement, callback with inner match, complete finite iterator sequence and frontier match. These are idioms, not deployed policies. Slash rewriting is not filesystem canonicalization. |
| BPATH255/256/4095 | Lua byte strings at the path helper's 256-byte/`PATH_MAX` buffer scales. The helper includes terminator space. These strings do not claim to be valid VFS component layouts or observed returned paths. |
| BX127/128 | Returned-value byte scales around `fs_xattr`'s current 128-byte buffer; not a claim that arbitrary larger xattrs can be read by this helper. |
| BEMPTY, BNUL, BZERO, BCAP32 | Empty/plain binary behavior, NUL classification, zero-width iteration and ordinary 32-capture compatibility at depth 65. |
| WPLAIN65534/65535/65536, WTAIL65535/65536 | Synthetic bulk data with exact unrestricted work `n+1`: below, equal to or beyond 65,536. These are not ordinary path/xattr sizes. |
| AOPT8/14, AGREEDY, AMINIMAL, ACLASS, ABALANCE, ACOMPARE, AOUTPUT, ADEPTH66, AMALFORMED | Finite derivatives of existing tests: optional branching, greedy/minimal retries, class/balance scans, capture comparison, output amplification, depth and syntax errors. |

The previous 36 vectors / 47 operation records remain the correctness baseline;
this corpus supplements them rather than replacing them. Its 44 samples add
policy-shaped inputs and explicit size/budget classes. It is intentionally not
a probability distribution. Equal sample weighting must not be presented as a
production mean or a production percentile. Actual application policies, hit/miss
frequencies, path/xattr distributions, acceptable rejection rates and latency
requirements have not been supplied. Ask the user for that judgment before a
compatibility tradeoff or final production recommendation depends on it.

## Measurement variants for the next task

Build all variants with the same compiler, base configuration, Lua core, depth
65 and source tree; record every test-only patch and object hash. No runtime
switch in the ordinary library is needed. The following comparison variants are
planned measurement work, not implemented by this generator:

| Variant | Purpose |
| --- | --- |
| A: charging disabled | Same current scanning/output helpers and depth guard; compile work begin/charge/end to no-ops in an isolated test build. Establish the incremental charging cost. It is not the historical pre-M3 implementation. |
| B: observe only | Same helpers, test observer counts every accepted event, enforcement disabled in a private copy. Collect complete finite operation demand and depth. Do not use its latency as production latency. |
| C: enforce | Unmodified production library, no observer; depth 65 and work 65,536. Measure ordinary success, near-limit success and rejection separately. |
| D: enforce and observe | Current private observation copy with production limits, plus test-only depth observations. Validate counters, rejection prefix and scopes, outside timing runs. |
| H: historical helpers | Additional matched reconstruction of post-M2/pre-M3 source with old plain/classification helpers, depth 65, no work charging. Compare H vs A to isolate the helper rewrite. |

A large override in the current fixture still executes debit/checks: it is not
variant A or a truly enforcement-free B. The observer at HEAD cannot report
maximum depth, so new private measurement support is required in the next task.
Historical M2 binaries alone cannot isolate overhead across different builds.
For H, preserve the exact historical source snapshot/patch and hash, rebuild with
the matched configuration, and document any unavoidable difference. A/B/H only
run in the supervised guest. Run finite safe sizes first; a timeout is a censored
result, never a zero or a discarded slow result. Do not start exponential size
expansion automatically. The v1 largest optional workload is n=14.

Do not derive a production overhead ratio by timing different workloads: use
identical inputs and compare successful A/C calls. A/B may finish work C rejects;
report their durations separately as different outcomes. For WPLAIN and WTAIL,
`n+1` analytically places the boundary; verify that formula in the measurement
copy. Other inputs qualify as near limit only after measured demand is within
[90%, 100%] of the current limit. Do not relabel small cases as near limit or
change the production limit just to put them there. Large normal outputs can
legitimately be rejected by the current candidate.

## Depth and work collection

Use a fresh C fixture per Lua state. Build inputs and closures before attaching
or resetting observations. Remove observations before result assertions and the
reuse probe. Never count those extra string calls as sample demand. Reserve
record capacity before entry; full/overflow makes a measurement invalid, not an
implicitly truncated success. Current capacity is 32 records. S05/BZERO have
four iterator calls including exhaustion; S04 has one outer gsub and two inner
matches. Keep operation order, parent operation ID and call ordinal explicitly.
Iterator sequences and callback totals may be summarized as descriptive sums,
but no shared budget is implied.

The primary operation row records:

- API, initial limit, remaining, all 18 named `SW_*` counters, total accepted
  charge, finished/exceeded/overflow/full flags and first rejected
  kind/cost/remaining.
- Peak admitted matcher depth (root is 1; plain is 0) and peak attempted depth.
  Record attempts at entry and admissions after work/depth guards, before body
  execution. A work refusal at SW_ENTER may precede the depth guard, including
  at attempted depth 66. Keep admitted depth distinct from an established C
  entry frame. `SW_ENTER` total is not maximum live depth.
- Outcome and result count/type/digest, including zero-result iterator completion.
  The JSON digest uses an explicit count plus values, not Lua table length.

Keep high-water observations in the fixture so longjmp cannot lose them. Nested
operations get their own maxima; also record the relation if reporting aggregate
stack activity. Do not infer whole task stack consumption from logical depth.
Accepted charge is requested logical work (e.g. precharged memcmp length), not
exact CPU instructions or exact bytes actually touched. A rejected call reports
its accepted prefix and failed request. It does not reveal total unrestricted
demand; `initial - remaining` is invalid as executed work after rejection zeroes
the balance. Get full demand from B only when that finite operation completes.

## Timing and raw data

Collect timing in actual x86_64 kernel builds under QEMU/KVM first; target
hardware is an additional environment. Separate timing runs from counters,
GDB, stack sampling, sanitizer runs and verbose result logging. No KUnit elapsed
value is a performance assertion.

1. Time a protected C invocation with `ktime_get_ns()` immediately around
   `lua_pcall`, retaining its result until the end timestamp. Preload function,
   arguments and required Lua stack space outside the interval. Measure an empty
   protected closure with the same harness to expose timer/dispatch overhead;
   report raw durations and the floor separately, without silently subtracting.
   Record actual timer resolution/clocksource; units alone do not prove accuracy.
2. A corpus operation closure gives a reproducible primary boundary that includes
   Lua dispatch and, for a sequence, all listed calls. If adding direct C-to-API
   timings, tag them as a separate boundary. Never pool sequence, single iterator,
   direct API and whole-policy timings. File lookup/LSM scheduling measurements
   are a separate end-to-end result, not string-library overhead.
3. Use five independent guest boots per variant. For successful cases, per boot
   use 100 untimed warmups and 2,000 individually timed observations in blocks of
   100. For expected errors, start with 5 warmups and 50 observations per boot;
   diagnostics can dominate, so do not claim p99 from that smaller population.
   All blocks remain under a host watchdog. If logging/time cost prevents this,
   preserve the partial run and record the revised protocol before more runs.
4. Reuse the same state within a block; start each block from a fresh fixture,
   run one full GC outside timing before warmup, then leave normal GC enabled.
   Keep callback construction/input setup outside timing, but their execution and
   normal allocation/GC inside the call count. Recreate consumed iterators outside
   each timed sequence (or prepare the specified single-call ordinal explicitly).
   Do not stop GC merely to improve the reported overhead. Report reused-input
   results as warm/interned; fresh-input experiments need a separate tag.
5. Rotate case and variant order deterministically across boots, save the order,
   and keep guest vCPU count/RAM/CPU model fixed. Record host CPU, kernel, QEMU,
   accelerator, compiler/version/flags, guest configs, frequency governor,
   clocksource, vCPU pinning, host load and competing workloads. Missing metadata
   is explicitly unknown. KVM timing includes host scheduling noise. Do not
   disable guest preemption/interrupts to force a cleaner task-context result.
6. Retain every duration as integer ns plus outcome. Report per sample/variant/
   boundary/boot count, mean, median, nearest-rank p95/p99 and maximum; compute
   quantiles from individual durations, not block averages. Only label p99 for
   populations of at least 1,000, and show per-boot spread and the finite sample
   count. No percentile establishes a worst-case bound. Outliers stay in data;
   any invalid run has a reason and remains exported.

Use UTF-8 JSONL, schema version 1. Timing and deterministic work records are
separate row kinds joined by sample/operation identity. Required common fields:
`run_id`, `boot_id`, `row_kind`, `corpus_version`, `manifest_sha256`,
`generator_sha256`, `sample_id`, `source_base`, `source_patch_sha256`,
`config_sha256`, `kernel_sha256`, `variant`, `boundary`, `environment_id`,
`subject_bytes`, `pattern_bytes`, `operation_id`, `parent_operation_id`,
`call_ordinal`, `outcome`, `valid`, `invalid_reason`.
Timing rows add `block_id`, `repeat_index`, `elapsed_ns`, `warmup`, `gc_mode`,
`result_digest`. Work rows add the counters/depth fields described above and
`complete_demand` (null for censored/rejected measurements).
Environment metadata and argv are immutable files referenced by hash. Use null
for unavailable measurements; do not fill planned fields with invented data.
Check unique row keys, result expectations, completeness, counter sums,
observation overflow, and unchanged input hashes before calculating statistics.

Normal corpus margin is reported per operation as complete demand / 65,536 and
peak admitted depth / 65, with maximum and range by workload group. Boundary and
pathological cases remain separate. Neither synthetic distributions nor the
current task-stack evidence justify increasing a limit. Final defaults, extra
KASAN/UBSAN/lockdep validation, architecture coverage and patch delivery remain
later M5 work. Limits on callbacks, repeated operations, VM execution, total
nested stack and fail-close remain outside this string boundary.
