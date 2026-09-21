# String limits: review and reproduction

The implementation at `e8b489ea83db766bd2e22fd80354d806828cc376` preserves the
internal depth/work limits of 65/65,536. It has x86_64 functional and selected
detection evidence; it is not a production performance or fail-close claim.
[Policy semantics and limitations](../../../security/lua/docs/STRING-LIMITS.md)
are separate from the test tools. The public review export is prepared against
`e12d58b59` (the parent of the first string-test commit), without rewriting the
development branch or its existing draft PR.

## Review units

The delivery's `patches/series` orders single-topic patches: behavior tests,
depth guard/tests, work observation/tests, enforcement/tests, iterator cursor
recovery, bounded plain scan, x86_64 KASAN longjmp cleanup, corpus, isolated
benchmark support, policy documentation, and QEMU reproduction/evidence docs.
Depth and observation are separated using the preserved post-M2 source, not a
new implementation. The final tree is checked against the development source.
Tests accompany their behavior change; the larger depth/observation test tables
remain cohesive rather than being divided at arbitrary line counts.

Suggested small PRs follow these units, with stacked bases where necessary.
The KASAN assembly fix is an independent PR against the existing setjmp base.
Corpus and benchmark tools need not be in the production-limit PR. Include each
patch's recorded validation and limitations, not all development history. Draft
PR text is supplied in the delivery's `REVIEW.md`; publishing or replacing the
existing draft PR is a separate action.

## Evidence attachment

The accompanying `lua-lsm-m5-evidence.tar.gz` extracts an `evidence/` directory.
Run `sha256sum -c SHA256SUMS` from that directory. `INVENTORY.json` maps each
included file to its original workspace-relative path, byte size and SHA-256.
The files are copied without editing historical identities. Inner historical
`SHA256SUMS` manifests may mention omitted binaries; use the attachment's outer
manifest to verify its contents. Kernel/source trees, object files, vmlinux and
bzImage are not shipped as review material; their recorded hashes remain in the
build identities. This is a reproducible source/evidence attachment, not a
prebuilt boot-image distribution.

| Question | Path relative to `evidence/` |
| --- | --- |
| Full open-API risk audit and original finite vectors | `.dev/LUA-API-AUDIT.md`, `.dev/STRING-PATTERN-TEST-VECTORS.md` |
| M4 two-configuration boundary/stack evidence | `.dev/sessions/2026-09-13-03-string-work-stack-validation.md`, `.build/m4-03/verification.txt` |
| Initial corpus protocol | `.build/m5-01/`, tracked [corpus README](corpus/README.md) |
| Full 1,262,250 timing samples and demand | `.build/m5-02/REPORT.md`, `stats.csv`, `timing-raw.jsonl.gz`, `work-raw.jsonl` in that directory |
| Plain optimization and both 220,500-sample comparisons | `.build/m5-03/REPORT.md`, `stats.csv`, `position-pair/stats.csv`, and each directory's `timing-raw.jsonl.gz` |
| Original KASAN failure, then bounds-strict failure | `.build/m5-04/detect-regression.log`, `.build/m5-04/fixed-detect-regression.log` |
| Final selected detection run | `.build/m5-04/REPORT.md`, `nobounds-detect-regression.log`, `nobounds-detect-entry66-regression.log` |
| Stack samples and source/build identities | `.build/m5-04/sample-summary.json`, `final-source-hashes.json`, `nobounds-build-identity.json` |
| Configurations used for reproduction | `configs/lean.config`, `configs/debug.config`, `configs/detect-nobounds.config`, `configs/detect-bounds-failed.config` |

Historical sessions/reports include local paths. Resolve them within the
attachment where present; references to omitted build artifacts describe their
original locations, not downloadable files. Do not rerun scripts that overwrite
archived outputs in place. Historical build identities are baseline commit plus
frozen patch, not retroactively relabeled as the current HEAD.

## Configurations and results

| Configuration | Distinct evidence and limits |
| --- | --- |
| Lean x86_64, 16 KiB task stack | M5-03: 39 cases, 3240 added scan checks, existing 813 quota checks, LSM regressions; performance collected separately with KUnit disabled |
| Old debug, 16 KiB | M4-03: frame-pointer unwinding, VMAP_STACK, DEBUG_STACK_USAGE, STACKPROTECTOR_STRONG; 38 cases and LSM/deep-error tests; minimum observed historical stack-write distance 2104 B |
| Detection, 32 KiB | M5-04: Generic KASAN OUTLINE/STACK/VMALLOC, UBSAN shift/div-zero/bool/enum, lockdep/PROVE_LOCKING, DEBUG_ATOMIC_SLEEP; 39 cases, LSM and 65/66-frame checks; minimum observed stack-write distance 12896 B |

The last configuration disables UBSAN_BOUNDS by an explicit scope decision.
Before the x86_64 longjmp fix, a caught error left poisoned stack frames and
later stack unwinding triggered KASAN. After that fix, the old closure tail-array
declaration triggered bounds-strict. Neither failed run is a PASS. The selected
configuration has three known `gsub` compiler frame warnings; these are not
hidden or equated to a clean all-sanitizer build. Stack-entry distances and
historical write low-water marks are not complete rsp extrema or arbitrary
nested-call guarantees. Other architectures and softirq are not covered here.

## Build and functional reproduction

Use an independent checkout with the complete review series applied, or the
recorded development HEAD plus the documentation/runner additions. Requirements
used historically: GCC 15.2.0, GNU make/binutils, QEMU 10.2.1 with KVM on
AMD Ryzen 7 5800H / WSL2. Install normal kernel build dependencies and a static
BusyBox. The configs enable Lua-LSM/debug/stats, built-in Lua/KUnit, initramfs,
serial console, securityfs and required pseudo filesystems, with modules off.

From that source root, create a new absolute output directory, choose one of
the attachment configs and build. The HOSTCFLAGS below are the recorded
host-tool compatibility flags; compiler or host changes require fresh evidence.

```sh
mkdir /tmp/lua-review-build
cp /path/to/evidence/configs/detect-nobounds.config /tmp/lua-review-build/.config
make O=/tmp/lua-review-build ARCH=x86_64 olddefconfig
make O=/tmp/lua-review-build ARCH=x86_64 CC=gcc HOSTCC=gcc \
  HOSTCFLAGS='-DO_LARGEFILE=0 -include /usr/include/byteswap.h -include /usr/include/endian.h' \
  -j8 bzImage
```

Record any `olddefconfig` changes and tool versions. A rebuilt image can differ
in build identity; do not require its whole binary hash to equal a historical
image or reuse old frame measurements for a different compiler/configuration.
Run the [QEMU entry points](qemu/README.md) in both modes, using fresh output
directories. The copied guest assertions cover smoke, nine depth decisions, six
work decisions, ten deep/bulk caught cases, four uncaught deep cases, and the
separate entry66 caught/uncaught cases, including unload recovery. Uncaught
errors intentionally verify the existing allow behavior, not fail-close.

## Corpus, performance and advanced experiments

Generate the finite corpus with [corpus/generate.py](corpus/generate.py) and
follow the [measurement protocol](corpus/README.md) and
[isolated benchmark instructions](benchmark/README.md). Benchmarks are not
linked by ordinary Kbuild. A/B/H can disable enforcement and must only run in a
watched guest. The exact historical M2 input is included at
`evidence/.build/m5-02/historical-lstrlib.c`; do not substitute a guessed revision.

For reproducing a historical experiment, use its `before.json` baseline and
`measurement-source.patch` (M5-03 also supplies `control-source.patch`) in a new
checkout. Copy its config and experimental host scripts into a **new** output
layout, recreate the `kernel/`, `source/` and initramfs paths they expect, and
adapt absolute workspace paths. Actual historical argv is in the per-run JSON.
M5-02's H is the historical M2 library; M5-03's H is the old enforced library.
They are different controls. The raw timing distributions, pilots, failed
wrappers, environment metadata and mean/median/p95/p99/max stay separate. Error
samples with only 250 observations do not report p99. No tail samples are
removed and EMPTY is not subtracted.

M5-03's ordinary-position pair improved WPLAIN65535 mean by about 75.5%, but
BPATH4095 mean increased about 11.6% (per-boot new/old 0.981–1.240). The cause
remains unresolved. No production workload or acceptable latency/rejection
threshold was supplied; the internal candidate decision is not production
acceptance. GDB sampling is separate from timings: archived focused sampling
uses the exact vmlinux, symbols, configuration and real task stack, with 600s
QEMU / 570s GDB timeouts. It is compiler-dependent and is not part of the portable
functional runner. Do not infer a new stack guarantee merely by replaying logs.
