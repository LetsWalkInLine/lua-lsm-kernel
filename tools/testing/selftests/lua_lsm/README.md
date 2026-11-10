# Lua-LSM selftests

This branch combines string limits (91395b488), policy error handling
(f73f03bf9) and VM deadlines (48725984e). The merge commit before the test
commits is the production baseline. Existing production PR branches are intact.
Rebase the test commits onto their merged equivalent when those PRs land.

Run on an x86_64 Linux host. Every policy runs in a disposable QEMU TCG guest
with a 300 second host timeout and a further 10 second kill deadline.
The suite takes several minutes. It never registers policies on the host.

## Build and run

Install a kernel build toolchain (GCC, binutils, make, flex, bison, bc, libelf
headers), Python 3, QEMU x86, cpio, coreutils timeout, a static x86_64 BusyBox,
static libc development files for the C probes, iproute2, and x86_64 Lua 5.1.
`ldd` must be available to package iproute2's loader and shared libraries.
BusyBox needs sh, mount, cat, grep, sleep, reboot and the usual file applets.
The kernel config enables securityfs, Lua-LSM statistics, KUnit, network
namespaces, veth and the receive observer. Run from the source root:

```sh
tools/testing/selftests/lua_lsm/build.sh "$PWD/.build/lua-selftest"
export LUA_LSM_KERNEL="$PWD/.build/lua-selftest/arch/x86/boot/bzImage"
export LUA_LSM_CONFIG="$PWD/.build/lua-selftest/.config"
export LUA_LSM_BUSYBOX=/bin/busybox
export LUA_LSM_OUTPUT="$PWD/.build/lua-selftest-results" # fresh directory
make -C tools/testing/selftests TARGETS=lua_lsm run_tests
```

`build.sh` expands the supplied minimal x86 config with olddefconfig; `JOBS` controls
parallelism. On hosts whose libc hides the legacy endian macros, use
`HOSTCFLAGS="-DO_LARGEFILE=0 -include /usr/include/byteswap.h -include /usr/include/endian.h"`
as an environment variable (the same compatibility flags used for validation).
It saves commit, source hashes, local source diff, config and image
hashes beside the build. No root or KVM is required. For one guest:

```sh
tools/testing/selftests/lua_lsm/run-qemu.py --mode network \
  --kernel "$LUA_LSM_KERNEL" --config "$LUA_LSM_CONFIG" \
  --busybox "$LUA_LSM_BUSYBOX" --output /tmp/lua-network-results
```

`regression.sh` emits TAP and returns 0 on success, 1 on failure, or kselftest
skip (4) if host prerequisites are missing. A booted guest missing an expected
kernel feature fails. Each output directory must be new. `run-qemu.py` returns
nonzero for watchdog/QEMU failure, diagnostics, failed cases, or missing success
markers. It saves console.log, exit-status and identity.json (kernel, initramfs,
BusyBox and test hashes, command line, source commit when run from a Git source
checkout, and config if supplied). Installed copies work without a source checkout or Git executable.
Keep the build manifest with these files: a git commit alone cannot identify
uncommitted build inputs. `--check-log` validates text only; it does not establish
kernel identity or QEMU exit status. `--prepare-only` builds an initramfs without
running it. `make install` and the kselftest runner retain the fixtures and tools.

## Coverage and organization

| Guest | Checks |
| --- | --- |
| kunit | 39 cases: string accounting/depth (14), errors/OOM (8), VM deadlines/contexts/recovery (10), private clock (2), return hooks (3), output ownership (1), combined strings (1) |
| dispatch | Real open(2), exact EIO/EACCES, ordinary caught errors, repeated same/other task recovery, lazy load retry and unload |
| budget | Real open(2), eight timeout families, uncatchable termination, ordinary caught errors, repeated recovery and lazy load retry |
| strings | Normal strings, caught work error, uncaught EIO, same task recovery; matcher active depth 65/66 across four APIs, 32 captures |
| dump32 | Valid 32-level registration/loading/export/execution, rejected 33-level registration without publication, caught export error and unload recovery |
| network | UDP across netns/veth, allowed packet, timed-out hook/drop, subsequent allowed receive after unload/reload |

C probes and Lua policies live in `fixtures/`; `open-check.h` is the shared exact
errno helper. `common.sh` mounts guest filesystems, tests the active LSM and a
real denial, and provides bounded unload retries. C probes are built with static
linking and warnings as errors when packaging. Ordinary quota rejection uses empty repetition so lazy initialization fits
within the new deadline. Work errors must remain
catchable with their original message; EIO alone cannot establish that property.
The old multi-MB backtracking and work-at-entry66 corpus is not counted as
covered: its initialization itself can exceed the deadline on TCG. Matcher
entry66 rejection is exercised with small inputs across all four APIs.

`generate-dump.py` compiles readable nested closures with host Lua 5.1 and
converts exact numeric constants to the kernel's integer ABI, checking chunk
layout and depth. It generates the input blobs only in the guest packaging
directory. This isolates dump traversal from the kernel parser's C-stack limit.

The test-only kretprobe in `security/lua/tests/receive_probe.c` logs the actual
receive dispatcher return and `in_serving_softirq()` context. The network checker
requires policy entry, Lua status 6, softirq return -EIO, and successful return
and delivery afterwards. Missing packets alone do not pass. The observer is
optional, has no production API, and belongs only in a disposable test kernel.
Network recovery exercises the receive path/per-CPU pool; KUnit separately
asserts exact VM ownership and state restoration.

KUnit tests live with their implementation: `lib/lua/tests/` uses a private
instrumented copy of the string library, and `security/lua/*kunit.c` exercises
production dispatchers and private budget-clock/converter copies. No production
observer is added. KUnit must also run under the host watchdog: several cases
intentionally loop forever unless the deadline implementation works.

Host packaging regressions can be checked without a kernel or QEMU:
`python3 tools/testing/selftests/lua_lsm/runner_test.py` (or `python3 runner_test.py`
in an installed copy).

## Add a case

Add deterministic accounting/boundary assertions to the appropriate KUnit case
list, then update expected suite counts in `check_log`. For an end-to-end case,
add a bounded input policy and exact result assertions to the relevant C probe;
check recovery immediately afterwards. Share helpers instead of duplicating
mount, unload or errno handling. Add a distinct guest marker only after all its
assertions pass and require it in `check_log`. A new guest mode also needs a TAP
plan entry in `regression.sh`. Preserve a negative check: missing markers,
nonzero exit, diagnostics and duplicate/incomplete plans must not report success.

## Separate benchmark entry

Functional regression disables `CONFIG_SECURITY_LUA_LSM_BENCHMARK`. The existing
experimental `vm_budget_bench.c` has its own off-by-default config and explicit
`benchmark.sh --kernel ... --busybox ... --output NEW_DIR` entry. Enable that
config and rebuild before using it. It is not part of the TAP regression or a
final performance report. Functional deadline tests keep termination assertions
without collecting the old 32-sample latency series. TCG timings do not establish
native overhead or latency percentiles.

The deadline is cooperative: C-only regions can return after the deadline.
These tests do not establish hard wall-time or arbitrary helper resource bounds.
This configuration targets x86_64 PREEMPT_LAZY; RT, other architectures,
KASAN/UBSAN and all-hook end-to-end coverage require separate validation.
