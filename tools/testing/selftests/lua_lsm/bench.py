#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host driver for the Lua-LSM benchmark delivery on QEMU/KVM.

Subcommands set up the three comparison trees (B0/B1/B2), build their
kernels, run guest boots and collect raw samples:

  setup   create .build/benchmark/trees/{b0,b1,b2} from git history plus
          the bench overlay, and record tree identities
  build   configure and build bzImage for one tree or all
  sys     run long-lived openat scenarios in fresh guests, interleaving
          kernels across rounds
  kern    run the in-kernel lua-lsm-bench suite (string corpus; VM
          deadline expiry with --expiry on budget trees)
  parse   re-parse existing boot directories
  stats   aggregate parsed runs into per-boot and pooled statistics

Every boot saves console.log, identity.json and parsed.json; integrity
is checked immediately and a failing boot aborts the batch.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
BUILD = REPO / ".build/benchmark"
FIXTURES = HERE / "bench"

B0_BASE = "f73f03bf9ac8635fb80171ea09dd55f0c08fcd25"
B1_STRING = "53b3c725695cbffece1cc2a764401b268ce9fd7a"
B2_COMBO = "52a5b2d0d0db1f66df98fb51756e7c581aaa552e"

KERNELS = {
    "b0": {
        "branch": "local/bench-b0",
        "desc": "error handling f73 + luaconf format ABI fix from 53b",
    },
    "b1": {
        "branch": "local/bench-b1",
        "desc": "f73 + full string branch 53b (includes the ABI fix)",
    },
    "b2": {
        "commit": B2_COMBO,
        "branch": None,
        "desc": "production combo 52a5b2d0d: string 53b + error f73 + budget 487",
    },
}

SYS_SCENARIOS = ["nolua", "nopolicy", "allow", "pathmatch", "loop32", "callback32"]
TIMEOUT_SCENARIO = "timeout"

# ncold, nwarmup, nsample, nblocks, blocksize
COUNTS_PILOT = "8,2000,2000,20,1000"
COUNTS_PILOT_TIMEOUT = "8,50,300,3,100"
COUNTS_FORMAL = "8,2000,10000,50,1000"
COUNTS_FORMAL_TIMEOUT = "8,50,400,3,100"


def sh(*argv, check=True, **kw):
    return subprocess.run(argv, check=check, text=True, capture_output=True, **kw)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(*argv, check=True):
    return sh("git", "-C", str(REPO), *argv, check=check)


def integration_hashes(tree):
    return {rel: digest(tree / rel)
            for rel in ("security/lua/Kconfig", "security/lua/Makefile",
                        "security/lua/bench_module.c")}


def verify_integration(names):
    """Refuse comparisons across mixed benchmark code versions: the
    trees of every kernel about to be built or compared must carry a
    byte-identical bench integration."""
    hashes = {}
    for name in names:
        tree = BUILD / "trees" / name
        if not tree.is_dir():
            raise SystemExit(f"missing benchmark tree for {name}; run setup")
        hashes[name] = integration_hashes(tree)
    first = hashes[names[0]]
    mixed = {n: h for n, h in hashes.items() if h != first}
    if mixed:
        raise SystemExit("mixed benchmark integration across kernels; "
                         f"re-run setup for all of {names}: {mixed}")


def verify_images(names):
    """Verify the images that will actually boot: each kernel needs a
    build record whose bzImage digest matches the file on disk and whose
    recorded integration hash matches the live tree, and every compared
    kernel must have been built from the same bench integration.  This
    catches stale images left behind when only some kernels were
    rebuilt after a source update."""
    seen = {}
    for name in names:
        out = BUILD / "build" / name
        rec_path = out / "build.json"
        bz = out / "arch/x86/boot/bzImage"
        if not rec_path.exists() or not bz.exists():
            raise SystemExit(f"missing kernel build for {name}; run build")
        rec = json.loads(rec_path.read_text())
        actual = digest(bz)
        if actual != rec.get("bzImage_sha256"):
            raise SystemExit(f"bzImage for {name} does not match its build "
                             f"record; rebuild it")
        live = integration_hashes(BUILD / "trees" / name)
        if rec.get("integration_sha256") != live:
            raise SystemExit(f"build record for {name} predates the current "
                             f"benchmark sources; rebuild it")
        seen[name] = rec.get("integration_sha256")
    first = seen[names[0]]
    mixed = {n: h for n, h in seen.items() if h != first}
    if mixed:
        raise SystemExit("kernels were built from different benchmark "
                         f"integrations; rebuild all of {names}: {mixed}")


# ---------------------------------------------------------------- setup

def make_b0():
    """Return commit hash of f73 + the 53b luaconf.h change."""
    branch = KERNELS["b0"]["branch"]
    if git("rev-parse", "--verify", branch, check=False).returncode == 0:
        return git("rev-parse", branch).stdout.strip()
    tmpdir = BUILD / "tmp-b0-worktree"
    if tmpdir.exists():
        shutil.rmtree(tmpdir)
    sh("git", "-C", str(REPO), "worktree", "add", "--detach", str(tmpdir), B0_BASE)
    try:
        blob = git("show", f"{B1_STRING}:include/linux/luaconf.h").stdout
        (tmpdir / "include/linux/luaconf.h").write_text(blob)
        ident = ["-c", "user.name=Lua-LSM benchmark",
                 "-c", "user.email=benchmark@lua-lsm.invalid"]
        env = dict(os.environ, GIT_COMMITTER_DATE="2026-11-16T00:00:00+00:00",
                   GIT_AUTHOR_DATE="2026-11-16T00:00:00+00:00")
        subprocess.run(["git", "-C", str(tmpdir), *ident, "commit",
                        "-am",
                        "lib/lua: match integer format argument types to ll conversions\n\n"
                        f"Benchmark B0 base: {B0_BASE} plus only the luaconf.h\n"
                        f"format ABI fix from {B1_STRING}."],
                       check=True, env=env, capture_output=True, text=True)
        commit = git("-C", str(tmpdir), "rev-parse", "HEAD").stdout.strip()
        git("branch", "-f", branch, commit)
    finally:
        sh("git", "-C", str(REPO), "worktree", "remove", "--force", str(tmpdir))
    return git("rev-parse", branch).stdout.strip()


def make_b1():
    """Return merge commit hash of f73 and the full string branch 53b."""
    branch = KERNELS["b1"]["branch"]
    if git("rev-parse", "--verify", branch, check=False).returncode == 0:
        return git("rev-parse", branch).stdout.strip()
    tmpdir = BUILD / "tmp-b1-worktree"
    if tmpdir.exists():
        shutil.rmtree(tmpdir)
    sh("git", "-C", str(REPO), "worktree", "add", "--detach", str(tmpdir), B0_BASE)
    try:
        # Complete author and committer identity: a clean checkout has
        # no repository-local git identity to fall back on.
        env = dict(os.environ, GIT_COMMITTER_DATE="2026-11-16T00:00:00+00:00",
                   GIT_AUTHOR_DATE="2026-11-16T00:00:00+00:00",
                   GIT_AUTHOR_NAME="Lua-LSM benchmark",
                   GIT_AUTHOR_EMAIL="benchmark@lua-lsm.invalid",
                   GIT_COMMITTER_NAME="Lua-LSM benchmark",
                   GIT_COMMITTER_EMAIL="benchmark@lua-lsm.invalid")
        subprocess.run(["git", "-C", str(tmpdir), "merge", "--no-edit", "-m",
                        "Merge string limits 53b3c7256 for benchmark B1\n\n"
                        "Benchmark B1 base: error handling f73 plus the full\n"
                        "string branch, matching B2 minus the budget commits.",
                        B1_STRING],
                       check=True, env=env, capture_output=True, text=True)
        commit = git("-C", str(tmpdir), "rev-parse", "HEAD").stdout.strip()
        git("branch", "-f", branch, commit)
    finally:
        sh("git", "-C", str(REPO), "worktree", "remove", "--force", str(tmpdir))
    return git("rev-parse", branch).stdout.strip()


def kernel_commit(name):
    if name == "b0":
        return make_b0()
    if name == "b1":
        return make_b1()
    if name == "b2":
        return KERNELS["b2"]["commit"]
    raise SystemExit("unknown kernel " + name)


def cmd_setup(args):
    BUILD.mkdir(parents=True, exist_ok=True)
    selected = [k for k in (args.kernels.split(",") if args.kernels
                            else ["b0", "b1", "b2"])]
    unknown = [k for k in selected if k not in KERNELS]
    if unknown or not selected:
        raise SystemExit(f"unknown kernels {unknown}; choose from {sorted(KERNELS)}")
    head = git("rev-parse", "HEAD").stdout.strip()
    setup_path = BUILD / "setup.json"
    identity = {"bench_branch_head": head, "kernels": {}}
    # A subset setup keeps the records of untouched kernels so later
    # build/run commands still resolve them.
    if setup_path.exists():
        old = json.loads(setup_path.read_text())
        for k, rec in old.get("kernels", {}).items():
            if k not in selected:
                rec = dict(rec)
                rec.setdefault("setup_head", old.get("bench_branch_head"))
                identity["kernels"][k] = rec
    bench_files = [
        "security/lua/bench_module.c",
        "tools/testing/selftests/lua_lsm/bench.config",
        *["tools/testing/selftests/lua_lsm/bench/" +
          str(p.relative_to(FIXTURES))
          for p in sorted(FIXTURES.rglob("*")) if p.is_file()],
    ]
    overlay = FIXTURES / "production-integration.patch"
    for name in selected:
        commit = kernel_commit(name)
        tree = BUILD / "trees" / name
        if tree.exists():
            shutil.rmtree(tree)
        tree.mkdir(parents=True)
        archive = subprocess.run(["git", "-C", str(REPO), "archive", commit],
                                 stdout=subprocess.PIPE, check=True).stdout
        subprocess.run(["tar", "-x", "-C", str(tree)], input=archive, check=True)
        # Copy bench files, then integrate into production Kconfig/Makefile.
        for rel in bench_files:
            dest = tree / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / rel, dest)
        sh("patch", "-d", str(tree), "-p1", "--fuzz=0", "--dry-run",
           stdin=open(overlay))
        sh("patch", "-d", str(tree), "-p1", "--fuzz=0", stdin=open(overlay))
        rec = {"commit": commit,
               "tree": git("rev-parse", commit + "^{tree}").stdout.strip(),
               "desc": KERNELS[name]["desc"], "setup_head": head}
        if name == "b0":
            diffstat = git("diff", "--stat", B0_BASE, commit).stdout
            assert "luaconf.h | 4" in diffstat, diffstat
            assert diffstat.count("file") == 1, diffstat
        if name == "b1":
            parents = git("rev-list", "--parents", "-n1", commit).stdout.split()
            assert len(parents) == 3 and parents[1] == B0_BASE \
                and parents[2] == B1_STRING, parents
        identity["kernels"][name] = rec
        print(f"{name}: commit {commit[:12]} tree {rec['tree'][:12]} ({rec['desc']})")
    # The applied integration must be identical on every selected tree;
    # records of untouched kernels are merged verbatim, not rechecked.
    # Each kernel record carries its own integration hash so mixed
    # versions are detectable before any comparison or build.
    hashes = {}
    for name in selected:
        hashes[name] = integration_hashes(BUILD / "trees" / name)
    baseline = hashes[selected[0]]
    assert all(h == baseline for h in hashes.values()), hashes
    for name in selected:
        identity["kernels"][name]["integration_sha256"] = baseline
    identity["integration_sha256"] = baseline
    (BUILD / "setup.json").write_text(json.dumps(identity, indent=2) + "\n")
    print("Setup complete:", BUILD / "setup.json")


# ---------------------------------------------------------------- build

def cmd_build(args):
    setup = json.loads((BUILD / "setup.json").read_text())
    selected = args.kernels.split(",") if args.kernels else ["b0", "b1", "b2"]
    verify_integration(selected)
    cc = os.environ.get("CC", "ccache gcc")
    # The tools/endian.h stub under scripts/include shadows the system
    # header and leaves BYTE_ORDER undefined, which silently breaks the
    # x86 relocs host tool; force-include the system headers first, as
    # in the documented historical builds.  O_LARGEFILE=0 matches the
    # kernel host-tool convention.
    hostcflags = "-DO_LARGEFILE=0 -include /usr/include/byteswap.h -include /usr/include/endian.h"
    for name in (args.kernels.split(",") if args.kernels else ["b0", "b1", "b2"]):
        src = BUILD / "trees" / name
        out = BUILD / "build" / name
        out.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(HERE / "bench.config", out / ".config")
        env = dict(os.environ)
        sh("make", "-C", str(src), f"O={out}", "olddefconfig", env=env)
        log = (out / "build.log").open("w")
        r = subprocess.run(["make", "-C", str(src), f"O={out}", f"-j{args.jobs}",
                            "CC=" + cc, "HOSTCFLAGS=" + hostcflags, "bzImage"],
                           env=env, stdout=log, stderr=subprocess.STDOUT)
        log.close()
        if r.returncode:
            raise SystemExit(f"build failed for {name}")
        # The compiler that kbuild actually probed, whatever CC names
        # (ccache wrapper, alternate gcc or clang).
        cc_version = "unknown"
        for line in (out / ".config").read_text().splitlines():
            if line.startswith("CONFIG_CC_VERSION_TEXT="):
                cc_version = line.split("=", 1)[1].strip('"')
                break
        bz = out / "arch/x86/boot/bzImage"
        rec = {"kernel": name, "commit": setup["kernels"][name]["commit"],
               "config_sha256": digest(out / ".config"),
               "bzImage_sha256": digest(bz),
               "integration_sha256": integration_hashes(src),
               "cc": cc, "gcc": cc_version,
               "built": time.strftime("%Y-%m-%dT%H:%M:%S")}
        (out / "build.json").write_text(json.dumps(rec, indent=2) + "\n")
        print(f"built {name}: {rec['bzImage_sha256'][:12]}")


# ---------------------------------------------------------------- images

def make_busybox_root(root, busybox):
    root.mkdir(parents=True)
    for name in ["bin", "dev", "proc", "sys", "tmp"]:
        (root / name).mkdir()
    (root / "tmp").chmod(0o1777)
    shutil.copyfile(busybox, root / "bin/busybox")
    (root / "bin/busybox").chmod(0o755)
    for applet in sh(str(busybox), "--list").stdout.split():
        if applet == "busybox" or Path(applet).name != applet:
            continue
        (root / "bin" / applet).symlink_to("busybox")


def build_image(kind, scenario, busybox):
    """Prepare the initramfs for a guest kind: sys-<scn> or kunit.

    Reuse only when the image exists AND every input that shapes it
    (busybox binary, init script, probe source, scenario policy) is
    byte-identical to the last build; the combined digest is kept
    beside the image.
    """
    name = f"{kind}" + (f"-{scenario}" if scenario else "")
    root = BUILD / "images" / name / "rootfs"
    image = BUILD / "images" / name / "initramfs.cpio.gz"
    stamp = BUILD / "images" / name / ".inputs.sha256"
    inputs = [busybox, FIXTURES / "init-bench", FIXTURES / "init-benchkunit",
              FIXTURES / "bench-syscall.c",
              *sorted((FIXTURES / "policies").glob("*.lua"))]
    inputs_sha = hashlib.sha256(
        "\n".join(f"{digest(p)}  {p.name}" for p in inputs).encode()).hexdigest()
    if image.exists() and stamp.exists() and stamp.read_text().strip() == inputs_sha:
        return image
    if root.exists():
        shutil.rmtree(root)
    make_busybox_root(root, busybox)
    if kind == "sys":
        shutil.copyfile(FIXTURES / "init-bench", root / "init")
        cc = sh("cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                str(FIXTURES / "bench-syscall.c"), "-o", str(root / "bench-syscall"),
                check=False)
        if cc.returncode:
            print(cc.stdout + cc.stderr)
            raise SystemExit("bench-syscall build failed")
        if scenario not in ("nopolicy", "nolua"):
            shutil.copyfile(FIXTURES / f"policies/{scenario}.lua",
                            root / "bench-policy.lua")
    else:
        shutil.copyfile(FIXTURES / "init-benchkunit", root / "init")
    (root / "init").chmod(0o755)
    names = ["."] + [str(p.relative_to(root)) for p in sorted(root.rglob("*"))]
    archive = subprocess.run(["cpio", "--null", "-o", "--format=newc", "--owner=0:0"],
                             cwd=root, input=("\0".join(names) + "\0").encode(),
                             stdout=subprocess.PIPE, check=True).stdout
    comp = subprocess.run(["gzip", "-n", "-9"], input=archive, stdout=subprocess.PIPE, check=True)
    image.write_bytes(comp.stdout)
    stamp.write_text(inputs_sha + "\n")
    return image


# ---------------------------------------------------------------- run

PREFIX = re.compile(r"^\[\s*\d+\.\d+\]\s*")


def host_snapshot():
    cpu = sh("lscpu").stdout
    gov = "unknown"
    try:
        gov = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read_text().strip()
    except OSError:
        pass
    driver = "unknown"
    try:
        driver = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver").read_text().strip()
    except OSError:
        pass
    return {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "uname": sh("uname", "-a").stdout.strip(),
        "lscpu_model": next(l.split(":", 1)[1].strip() for l in cpu.splitlines()
                            if l.startswith("Model name")),
        "cpufreq_governor": gov,
        "cpufreq_driver": driver,
        "loadavg": Path("/proc/loadavg").read_text().strip(),
        "qemu": sh("qemu-system-x86_64", "--version").stdout.splitlines()[0],
    }


def run_qemu(out_dir, kernel, image, append, timeout_s, pin):
    out_dir.mkdir(parents=True, exist_ok=False)
    argv = ["taskset", "-c", pin,
            "timeout", "--foreground", "--kill-after=10s", f"{timeout_s}s",
            "qemu-system-x86_64",
            "-machine", "pc,accel=kvm", "-cpu", "host",
            "-smp", "2", "-m", "2048", "-nodefaults", "-no-reboot",
            "-display", "none", "-monitor", "none", "-serial", "stdio",
            "-kernel", str(kernel), "-initrd", str(image),
            "-append", append]
    with (out_dir / "console.log").open("x") as log:
        r = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    (out_dir / "exit-status").write_text(str(r.returncode) + "\n")
    return r.returncode, argv


def boot_identity(out_dir, kernel_name, bz, image, argv, extra):
    setup = json.loads((BUILD / "setup.json").read_text())
    build = json.loads((BUILD / "build" / kernel_name / "build.json").read_text())
    ident = {"kernel": kernel_name, "commit": setup["kernels"][kernel_name]["commit"],
             "bzImage_sha256": digest(bz), "initramfs_sha256": digest(image),
             "config_sha256": build["config_sha256"],
             "argv": argv, "host": host_snapshot(), "extra": extra}
    (out_dir / "identity.json").write_text(json.dumps(ident, indent=2) + "\n")
    return ident


def read_log(path):
    lines = []
    for raw in Path(path).read_text(errors="replace").splitlines():
        lines.append(PREFIX.sub("", raw))
    return lines


def check_common(log_lines):
    text = "\n".join(log_lines)
    if re.search(r"BUG:|WARNING:|Oops:|Kernel panic|KASAN:|UBSAN:|FAIL:", text):
        raise ValueError("guest failure or diagnostic in log")
    if "KVM" not in text:
        raise ValueError("no KVM evidence in boot log")


def values_from_raw(parts):
    out = []
    for part in sorted(parts, key=lambda p: p[0]):
        out.extend(part[2])
    return out


def parse_sys(log_lines, scenario, counts):
    ncold, nwarm, nsample, nblocks, bsize = map(int, counts.split(","))
    cold = []
    stats = {}
    raw = {"samples": [], "blocks": []}
    done = False
    for line in log_lines:
        if line.startswith("BENCH_COLD tag="):
            m = re.match(r"BENCH_COLD tag=\S+ i=(\d+) ns=(\d+) ret=(-?\d+) errno=(\d+)", line)
            if not m:
                raise ValueError("bad cold line: " + line)
            cold.append({"i": int(m.group(1)), "ns": int(m.group(2)),
                         "ret": int(m.group(3)), "errno": int(m.group(4))})
        elif line.startswith("BENCH_STATS tag="):
            m = re.match(r"BENCH_STATS tag=(\S+) n=(\d+) mean=([\d.]+) stdev=([\d.]+) "
                         r"min=(\d+) p50=(\d+) p90=(\d+) p95=(\d+) p99=(\d+) max=(\d+)", line)
            if not m:
                raise ValueError("bad stats line: " + line)
            stats[m.group(1)] = {k: float(v) if "." in v else int(v)
                                 for k, v in zip(
                                     ["n", "mean", "stdev", "min", "p50", "p90",
                                      "p95", "p99", "max"], m.groups()[1:])}
        elif line.startswith("BENCH_RAW tag="):
            m = re.match(r"BENCH_RAW tag=(\S+) part=(\d+)/(\d+)(?: (.*))?", line)
            if not m:
                raise ValueError("bad raw line: " + line)
            vals = [int(x) for x in (m.group(4) or "").split(",") if x]
            raw[m.group(1)].append((int(m.group(2)), int(m.group(3)), vals))
        elif line.startswith("BENCH_SYS tag=") and " DONE " in line:
            done = True
    if not done:
        raise ValueError("bench-syscall did not finish")
    if len(cold) != ncold:
        raise ValueError(f"cold count {len(cold)} != {ncold}")
    for tag, want in (("samples", nsample), ("blocks", nblocks)):
        parts = raw[tag]
        totals = {total for _, total, _ in parts}
        idx = sorted(i for i, _, _ in parts)
        if not parts or len(totals) != 1 or idx != list(range(totals.pop())):
            raise ValueError(f"{tag} raw parts are incomplete or duplicated: {idx}")
        if tag not in stats:
            raise ValueError(f"missing stats line for {tag}")
    samples = values_from_raw(raw["samples"])
    blocks = values_from_raw(raw["blocks"])
    if len(samples) != nsample:
        raise ValueError(f"sample count {len(samples)} != {nsample}")
    if len(blocks) != nblocks:
        raise ValueError(f"block count {len(blocks)} != {nblocks}")
    expect_eio = scenario == TIMEOUT_SCENARIO
    for c in cold:
        if expect_eio and not (c["ret"] < 0 and c["errno"] == 5):
            raise ValueError("cold call did not fail with EIO: " + json.dumps(c))
        if not expect_eio and c["ret"] < 0:
            raise ValueError("cold call failed: " + json.dumps(c))
    return {"cold": [c["ns"] for c in cold], "samples": samples,
            "blocks": blocks, "stats": stats,
            "counts": counts, "bsize": bsize}


def validate_sys(parsed, log_lines, scenario):
    text = "\n".join(log_lines)
    if text.count("LUA_BENCH: PASS") != 1:
        raise ValueError("missing or duplicate LUA_BENCH: PASS")
    active = re.search(r"LUA_BENCH: lsm: (.*)", text)
    if not active:
        raise ValueError("no active LSM line")
    has_lua = ",lua," in f",{active.group(1).strip()},"
    if scenario == "nolua" and has_lua:
        raise ValueError("lua active in nolua scenario: " + active.group(1))
    if scenario != "nolua" and not has_lua:
        raise ValueError("lua not active: " + active.group(1))


def cmd_sys(args):
    scenarios = args.scenarios.split(",") if args.scenarios else SYS_SCENARIOS
    if args.timeout:
        scenarios = scenarios + [TIMEOUT_SCENARIO]
    kernels = args.kernels.split(",") if args.kernels else ["b0", "b1", "b2"]
    verify_integration(kernels)
    verify_images(kernels)
    busybox = Path(args.busybox)
    bz = {k: BUILD / "build" / k / "arch/x86/boot/bzImage" for k in kernels}
    for k, p in bz.items():
        if not p.exists():
            raise SystemExit(f"missing kernel build for {k}")
    runs = Path(args.runs)
    runs.mkdir(parents=True, exist_ok=True)
    boot = 0
    diagnostics = 0
    for rnd in range(args.rounds):
        for si, scn in enumerate(scenarios):
            off = (rnd + si) % len(kernels)
            order = kernels[off:] + kernels[:off]
            for kernel in order:
                if scn == TIMEOUT_SCENARIO and kernel != "b2":
                    continue
                image = build_image("sys", scn, busybox)
                counts = (args.counts_timeout if scn == TIMEOUT_SCENARIO
                          else args.counts)
                append = ("console=ttyS0,115200 rdinit=/init panic=-1 oops=panic "
                          "panic_on_warn=1 nokaslr kunit.enable=0 "
                          f"lua_bench_scn={scn} lua_bench_counts={counts}")
                if scn == "nolua":
                    append = append.replace("nokaslr", "nokaslr lsm=landlock")

                def once(attempt, kernel=kernel, scn=scn, image=image,
                         counts=counts, append=append, rnd=rnd):
                    nonlocal boot
                    boot += 1
                    out = runs / f"boot-{boot:03d}-{kernel}-{scn}"
                    code, argv = run_qemu(out, bz[kernel], image, append,
                                          args.timeout_s, args.pin)
                    if code:
                        raise SystemExit(f"QEMU failed for {kernel}/{scn}: {code}")
                    try:
                        lines = read_log(out / "console.log")
                        check_common(lines)
                        parsed = parse_sys(lines, scn, counts)
                        validate_sys(parsed, lines, scn)
                    except ValueError as e:
                        klass = classify_failure(lines)
                        record_event(runs, out.name,
                                     {"class": klass, "kernel": kernel, "kind": "sys",
                                      "scenario": scn, "round": rnd, "reason": str(e)})
                        if klass in ("measurement", "diagnostic"):
                            shutil.move(str(out), str(out) + f"-{klass}")
                            raise MeasurementEvent(f"{kernel}/{scn} [{klass}]: {e}")
                        shutil.move(str(out), str(out) + f"-failed{attempt}")
                        print(f"acquisition failure ({e}); kept evidence")
                        return None
                    boot_identity(out, kernel, bz[kernel], image, argv,
                                  {"kind": "sys", "scenario": scn, "counts": counts,
                                   "round": rnd})
                    (out / "parsed.json").write_text(json.dumps(parsed) + "\n")
                    return parsed, out

                try:
                    parsed, out = boot_with_retry(f"{kernel}/{scn}", once)
                except MeasurementEvent as e:
                    print(f"measurement event kept, boot skipped: {e}")
                    if "[diagnostic]" in str(e):
                        diagnostics += 1
                    continue
                s = parsed["stats"]
                print(f"[{boot:03d}] r{rnd} {kernel}/{scn}: "
                      f"sample mean {s['samples']['mean']:.0f}ns "
                      f"p95 {s['samples']['p95']:.0f}ns "
                      f"block {s['blocks']['mean'] / parsed['bsize']:.0f}ns/op")
    if diagnostics:
        raise SystemExit(f"{diagnostics} sys boot(s) hit kernel diagnostics; "
                         "see run-events.jsonl and the kept -diagnostic logs")


OVER_ITEMS = {"escapeover76191", "findover76191", "trimsp1026", "basename4095"}


def _finalize_parts(rec, what):
    """Assemble raw samples from their part lines, verifying the part
    numbers form exactly 0..N-1 with one consistent count."""
    vals_by_idx = rec.pop("_partvals", None)
    totals = rec.pop("_totals", set())
    if not vals_by_idx:
        raise ValueError(f"missing raw sample lines for {what}")
    if len(totals) != 1:
        raise ValueError(f"inconsistent part counts for {what}: {sorted(totals)}")
    total = totals.pop()
    if sorted(vals_by_idx) != list(range(total)):
        raise ValueError(f"part numbers for {what} are not exactly "
                         f"0..{total - 1}: {sorted(vals_by_idx)}")
    rec["raw"] = [v for i in range(total) for v in vals_by_idx[i]]


def parse_kern(log_lines, expect_expiry):
    corpus, expiry = [], []
    cur = None
    last_recovery_load = None
    done = set()

    def close(rec, what):
        if rec is None:
            return
        if "stats" not in rec:
            raise ValueError(f"missing BENCH_STATS line for {what}")
        _finalize_parts(rec, what)

    for line in log_lines:
        if "BENCH_INFO phase=corpus" in line:
            close(cur, "record")
            cur = None
        elif "BENCH_INFO phase=expiry" in line:
            close(cur, "record")
            cur = None
        elif "BENCH_DONE phase=" in line:
            done.add(line.split("phase=", 1)[1].strip())
        elif "BENCH_STR name=" in line:
            m = re.search(r"BENCH_STR name=(\S+) v=(\S+) demand=(\d+) cat=(-?\d+) cold_ns=(\d+)", line)
            if not m:
                raise ValueError("bad BENCH_STR: " + line)
            close(cur, "previous record")
            cur = {"kind": "str", "name": m.group(1), "variant": m.group(2),
                   "demand": int(m.group(3)), "cat": int(m.group(4)),
                   "cold_ns": int(m.group(5)), "_partvals": {}, "_totals": set()}
            corpus.append(cur)
        elif "BENCH_EXP name=" in line:
            m = re.search(r"BENCH_EXP name=(\S+) tag=(\S+) ctx=(\S+)"
                          r"(?: v=(\S+))?(?: cold_ns=(\d+))?"
                          r"(?: budget=(\d+))?(?: allow_mean=(\d+))?", line)
            if not m:
                raise ValueError("bad BENCH_EXP: " + line)
            name, tag, ctx = m.group(1), m.group(2), m.group(3)
            if name == "allow" and m.group(5):
                close(cur, "previous record")
                cur = {"kind": "exp_allow", "shape": tag, "ctx": ctx,
                       "cold_ns": int(m.group(5)), "_partvals": {}, "_totals": set()}
                expiry.append(cur)
            elif name == "allow":
                if (cur is None or cur.get("kind") != "exp_allow"
                        or cur["shape"] != tag or cur["ctx"] != ctx):
                    raise ValueError(f"allow warm line without its cold header: {line}")
                cur["warm_done"] = True
            elif name == "recovery-load":
                # Informational untimed lazy-load call in preemptible
                # context; its cold time is attached to the recovery
                # record that follows.
                last_recovery_load = int(m.group(5) or 0)
                close(cur, "previous record")
                cur = None
            elif name == "recovery":
                close(cur, "previous record")
                cur = {"kind": "exp_recovery", "shape": tag, "ctx": ctx,
                       "load_cold_ns": last_recovery_load,
                       "_partvals": {}, "_totals": set()}
                last_recovery_load = None
                expiry.append(cur)
            else:
                close(cur, "previous record")
                cur = {"kind": "exp_timeout", "shape": tag, "ctx": ctx,
                       "budget": int(m.group(6) or 0),
                       "allow_mean": int(m.group(7) or 0),
                       "_partvals": {}, "_totals": set()}
                expiry.append(cur)
        elif "BENCH_STATS n=" in line:
            m = re.search(r"BENCH_STATS n=(\d+) mean=(\d+) min=(\d+) max=(\d+)", line)
            if not m:
                raise ValueError("bad stats: " + line)
            if cur is not None:
                cur["stats"] = {"n": int(m.group(1)), "mean": int(m.group(2)),
                                "min": int(m.group(3)), "max": int(m.group(4))}
        elif "BENCH_STR_RAW" in line or "BENCH_EXP_RAW" in line:
            m = re.search(r"part=(\d+)/(\d+)(?: (.*))?$", line)
            if not m or cur is None:
                continue
            idx, total = int(m.group(1)), int(m.group(2))
            if idx in cur["_partvals"]:
                raise ValueError(f"duplicate part {idx} for current record")
            cur["_partvals"][idx] = [int(x) for x in (m.group(3) or "").split(",") if x]
            cur["_totals"].add(total)
    close(cur, "last record")
    if "corpus" not in done:
        raise ValueError("corpus phase did not complete")
    if expect_expiry and "expiry" not in done:
        raise ValueError("expiry phase did not complete")
    return {"corpus": corpus, "expiry": expiry}


CORPUS_VARIANTS = {
    "allow": ("raw",),
    "lower4096": ("raw", "caught"),
    "labelmatch65536": ("raw", "caught"),
    "replace65536": ("raw", "caught"),
    "findnear76190": ("raw", "caught"),
    "escapenear76190": ("raw", "caught"),
    "escapeover76191": ("raw", "caught"),
    "findover76191": ("raw", "caught"),
    "trimsp1026": ("raw", "caught"),
    "basename4095": ("raw", "caught"),
}
EXPIRY_KINDS = ("exp_allow", "exp_timeout", "exp_recovery")
EXPIRY_COUNTS = {"exp_allow": 32, "exp_timeout": 32, "exp_recovery": 16}


def validate_kern(parsed, kernel, expect_expiry):
    limit = kernel in ("b1", "b2")
    seen = {}
    for rec in parsed["corpus"]:
        key = (rec["name"], rec["variant"])
        if key in seen:
            raise ValueError(f"duplicate corpus record {key}")
        seen[key] = rec
    expected = {(name, v) for name, variants in CORPUS_VARIANTS.items()
                for v in variants}
    missing = expected - set(seen)
    extra = set(seen) - expected
    if missing or extra:
        raise ValueError(f"corpus records mismatch: missing={sorted(missing)} "
                         f"extra={sorted(extra)}")
    for key, rec in seen.items():
        if len(rec["raw"]) != 128 or rec["stats"]["n"] != 128:
            raise ValueError(f"bad warm count for {key}")
        name, variant = key
        cat = rec["cat"]
        if variant == "raw":
            if name in OVER_ITEMS and limit:
                if cat != -5:
                    raise ValueError(f"{name} raw cat {cat} != -EIO on {kernel}")
            elif cat != 0:
                raise ValueError(f"{name} raw cat {cat} != 0")
        else:
            if name in OVER_ITEMS and limit:
                if cat != -16:
                    raise ValueError(f"{name} caught cat {cat} != -EBUSY")
            elif cat != 0:
                raise ValueError(f"{name} caught cat {cat} != 0")
    if expect_expiry:
        shapes = {"loop", "pcall", "coroutine", "callback"}
        exp_seen = {}
        for rec in parsed["expiry"]:
            key = (rec["shape"], rec["ctx"], rec["kind"])
            if key in exp_seen:
                raise ValueError(f"duplicate expiry record {key}")
            exp_seen[key] = rec
        want = {(s, c, k) for s in shapes for c in ("task", "atomic")
                for k in EXPIRY_KINDS}
        missing = want - set(exp_seen)
        extra = set(exp_seen) - want
        if missing or extra:
            raise ValueError(f"expiry records mismatch: missing={sorted(missing)} "
                             f"extra={sorted(extra)}")
        for key, rec in exp_seen.items():
            n = EXPIRY_COUNTS[key[2]]
            if len(rec["raw"]) != n or rec["stats"]["n"] != n:
                raise ValueError(f"bad sample count for expiry {key}: "
                                 f"{len(rec['raw'])} != {n}")
        shapes_seen = {r["shape"] for r in parsed["expiry"]}
        if shapes_seen != shapes:
            raise ValueError(f"expiry shapes mismatch: {sorted(shapes_seen ^ shapes)}")


class MeasurementEvent(Exception):
    """A validation failure caused by a real measured outcome, not by a
    broken acquisition: e.g. a host stall flipping a budgeted call to a
    timeout mid-sample.  Recorded, never silently retried."""


# Kernel-level diagnostics dominate every other classification: a boot
# with BUG/WARNING/Oops/panic is never a usable measurement, even when
# flip markers are present.
KERNEL_DIAGNOSTIC_PATTERNS = (
    re.compile(r"(?:BUG:|WARNING:|Oops:|Kernel panic|KASAN:|UBSAN:)"),
)

# Console markers that identify a genuine measurement flip: the kernel
# module's per-call consistency assertion, or a userspace sample whose
# return category changed under the VM deadline.
FLIP_PATTERNS = (
    "Expected ret == first",
    re.compile(r"BENCH_(?:COLD|WARMUP|SAMPLE|THROUGH)[^\n]*FAIL"),
)

# KUnit assertion failures are product or setup problems, never
# acquisition noise, and must not be auto-retried away.
KUNIT_FAILURE_PATTERNS = (
    re.compile(r"(?:EXPECTATION|ASSERTION) FAILED"),
    re.compile(r"\bnot ok \d"),
)


def classify_failure(log_lines):
    """Classify a failed boot: diagnostic, measurement or acquisition.

    Only 'acquisition' boots are auto-retried.  Kernel diagnostics win
    over flip markers; flips win over KUnit markers because their boots
    legitimately carry 'not ok' results."""
    text = "\n".join(log_lines)
    for pat in KERNEL_DIAGNOSTIC_PATTERNS:
        if pat.search(text):
            return "diagnostic"
    for pat in FLIP_PATTERNS:
        if isinstance(pat, str):
            if pat in text:
                return "measurement"
        elif pat.search(text):
            return "measurement"
    for pat in KUNIT_FAILURE_PATTERNS:
        if pat.search(text):
            return "diagnostic"
    return "acquisition"


def record_event(runs, name, entry):
    path = Path(runs) / "run-events.jsonl"
    with path.open("a") as f:
        f.write(json.dumps({"boot": name, **entry}) + "\n")


def boot_with_retry(desc, boot_once, attempts=3):
    """Run one guest boot, retrying on validation failure.

    Host stalls (unpinned frequency, descheduling) can legitimately
    flip a budgeted call to a timeout mid-sample; the strict per-boot
    validation catches that.  Failed boots are kept on disk as
    <dir>-failed<N> evidence and retried on a fresh guest.
    """
    for attempt in range(attempts):
        out = boot_once(attempt)
        if out is not None:
            return out
        print(f"retry: {desc} boot failed validation "
              f"(attempt {attempt + 1}/{attempts})")
    raise SystemExit(f"boot kept failing validation after {attempts} attempts: {desc}")


def cmd_kern(args):
    kernels = args.kernels.split(",") if args.kernels else ["b0", "b1", "b2"]
    verify_integration(kernels)
    verify_images(kernels)
    busybox = Path(args.busybox)
    image = build_image("kunit", None, busybox)
    runs = Path(args.runs)
    runs.mkdir(parents=True, exist_ok=True)
    boot = 0
    diagnostics = 0
    for rnd in range(args.rounds):
        order = kernels[rnd % len(kernels):] + kernels[:rnd % len(kernels)]
        for kernel in order:
            want_expiry = args.expiry and kernel == "b2"
            bz = BUILD / "build" / kernel / "arch/x86/boot/bzImage"
            if not bz.exists():
                raise SystemExit(f"missing kernel build for {kernel}")
            append = ("console=ttyS0,115200 rdinit=/init panic=-1 oops=panic "
                      "panic_on_warn=1 nokaslr lsm=lua "
                      "kunit.filter_glob=lua-lsm-bench")
            if want_expiry:
                append += " lua_bench_expiry"

            def once(attempt, kernel=kernel, want_expiry=want_expiry, bz=bz,
                     append=append):
                nonlocal boot
                boot += 1
                out = runs / (f"boot-{boot:03d}-{kernel}-kern"
                              f"{'-exp' if want_expiry else ''}")
                code, argv = run_qemu(out, bz, image, append, args.timeout_s, args.pin)
                if code:
                    raise SystemExit(f"QEMU failed for {kernel}: {code}")
                try:
                    lines = read_log(out / "console.log")
                    check_common(lines)
                    parsed = parse_kern(lines, want_expiry)
                    validate_kern(parsed, kernel, want_expiry)
                except ValueError as e:
                    klass = classify_failure(lines)
                    record_event(runs, out.name,
                                 {"class": klass, "kernel": kernel, "kind": "kern",
                                  "expiry": want_expiry, "round": rnd, "reason": str(e)})
                    if klass in ("measurement", "diagnostic"):
                        shutil.move(str(out), str(out) + f"-{klass}")
                        raise MeasurementEvent(f"{kernel} kern [{klass}]: {e}")
                    shutil.move(str(out), str(out) + f"-failed{attempt}")
                    print(f"acquisition failure ({e}); kept evidence")
                    return None
                boot_identity(out, kernel, bz, image, argv,
                              {"kind": "kern", "expiry": want_expiry, "round": rnd})
                (out / "parsed.json").write_text(json.dumps(parsed) + "\n")
                return parsed, out

            try:
                parsed, out = boot_with_retry(f"{kernel} kern", once)
            except MeasurementEvent as e:
                print(f"measurement event kept, boot skipped: {e}")
                if "[diagnostic]" in str(e):
                    diagnostics += 1
                continue
            first = next(r for r in parsed["corpus"] if r["name"] != "allow")
            print(f"[{boot:03d}] r{rnd} {kernel} kern: "
                  f"{len(parsed['corpus'])} corpus recs, {len(parsed['expiry'])} expiry recs, "
                  f"e.g. {first['name']}/{first['variant']} mean {first['stats']['mean']}ns")
    if diagnostics:
        raise SystemExit(f"{diagnostics} kern boot(s) hit kernel diagnostics; "
                         "see run-events.jsonl and the kept -diagnostic logs")

# ---------------------------------------------------------------- stats

def percentile(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def summarize(v):
    if not v:
        return None
    mean = sum(v) / len(v)
    var = sum((x - mean) ** 2 for x in v) / len(v)
    return {"n": len(v), "mean": mean, "stdev": math.sqrt(var),
            "min": min(v), "p50": percentile(v, .5), "p95": percentile(v, .95),
            "p99": percentile(v, .99), "max": max(v)}


def load_runs(runs):
    out = []
    for d in sorted(Path(runs).glob("boot-*")):
        pj = d / "parsed.json"
        ij = d / "identity.json"
        if pj.exists() and ij.exists():
            out.append((d.name, json.loads(ij.read_text()), json.loads(pj.read_text())))
    if not out:
        print("warning: no parsed boots under", runs)
    return out


def cmd_parse(args):
    for d in sorted(Path(args.runs).glob("boot-*")):
        if not (d / "console.log").exists() or not (d / "identity.json").exists():
            continue  # failed-boot evidence without identity stays untouched
        ident = json.loads((d / "identity.json").read_text())
        lines = read_log(d / "console.log")
        if ident["extra"]["kind"] == "sys":
            parsed = parse_sys(lines, ident["extra"]["scenario"], ident["extra"]["counts"])
            validate_sys(parsed, lines, ident["extra"]["scenario"])
        else:
            parsed = parse_kern(lines, ident["extra"]["expiry"])
            validate_kern(parsed, ident["kernel"], ident["extra"]["expiry"])
        (d / "parsed.json").write_text(json.dumps(parsed) + "\n")
        print("parsed", d.name)


def cmd_stats(args):
    runs = load_runs(args.runs)
    report = {"sys": {}, "kern_corpus": {}, "kern_expiry": {}}
    events_path = Path(args.runs) / "run-events.jsonl"
    if events_path.exists():
        events = [json.loads(l) for l in events_path.read_text().splitlines()
                  if l.strip()]
        grouped = {}
        for e in events:
            grouped.setdefault(f"{e.get('kernel')}/{e['class']}", []).append(e["boot"])
        report["run_events"] = {
            "total": len(events),
            "by_kernel_and_class": {k: len(v) for k, v in sorted(grouped.items())},
            "boots": events,
        }
    if not runs and "run_events" not in report:
        raise SystemExit(f"no parsed boots and no run events under {args.runs}")
    sysdata = {}
    for name, ident, parsed in runs:
        if ident["extra"]["kind"] != "sys":
            continue
        key = (ident["kernel"], ident["extra"]["scenario"])
        sysdata.setdefault(key, []).append((ident["extra"].get("round"), parsed))
    for (kernel, scn), boots in sorted(sysdata.items()):
        entry = {}
        for field in ("cold", "samples"):
            per_boot = [sum(b[field]) / len(b[field]) for _, b in boots]
            pooled = [x for _, b in boots for x in b[field]]
            entry[field] = {"per_boot_mean": per_boot, "pooled": summarize(pooled)}
        thr = [x / b["bsize"] for _, b in boots for x in b["blocks"]]
        thr_boot = [sum(b["blocks"]) / (len(b["blocks"]) * b["bsize"]) for _, b in boots]
        entry["throughput"] = {"per_boot_mean": thr_boot, "pooled": summarize(thr)}
        report["sys"][f"{kernel}/{scn}"] = entry
    incs = {}
    for scn in sorted({k[1] for k in sysdata}):
        for old, new in (("b0", "b1"), ("b1", "b2"), ("b0", "b2")):
            kold, knew = sysdata.get((old, scn)), sysdata.get((new, scn))
            if not kold or not knew:
                continue
            # Pair boots of the two kernels by round: per-round ratios
            # must keep the actual round correspondence, not sort both
            # sides independently.
            mold = {r: sum(x["samples"]) / len(x["samples"]) for r, x in kold}
            mnew = {r: sum(x["samples"]) / len(x["samples"]) for r, x in knew}
            common = sorted(set(mold) & set(mnew))
            lold = [x for _, b in kold for x in b["samples"]]
            lnew = [x for _, b in knew for x in b["samples"]]
            # Latency deltas come from the commonly valid rounds only,
            # so extra successful boots on one side cannot skew the
            # comparison when the other side skipped a round.
            pair_deltas = [mnew[r] - mold[r] for r in common]
            incs[f"{scn}:{new}-{old}"] = {
                "delta_common_rounds_ns": (sum(pair_deltas) / len(pair_deltas)
                                           if pair_deltas else None),
                "common_rounds": len(common),
                "delta_boot_mean_ns": (sum(mnew.values()) / len(mnew)) - (sum(mold.values()) / len(mold)),
                "ratio_pooled": (sum(lnew) / len(lnew)) / (sum(lold) / len(lold)),
                "per_round_ratios": {str(r): mnew[r] / mold[r] for r in common},
            }
    report["sys_increments"] = incs
    kdata = {}
    for name, ident, parsed in runs:
        if ident["extra"]["kind"] != "kern":
            continue
        for rec in parsed["corpus"]:
            kdata.setdefault((ident["kernel"], rec["name"], rec["variant"]), []).append(rec)
    for (kernel, item, variant), recs in sorted(kdata.items()):
        warm = [x for r in recs for x in r["raw"]]
        report["kern_corpus"][f"{kernel}/{item}/{variant}"] = {
            "cat": recs[0]["cat"], "demand": recs[0]["demand"],
            "cold": summarize([r["cold_ns"] for r in recs]),
            "warm": summarize(warm),
            "per_boot_mean": [r["stats"]["mean"] for r in recs],
        }
    edata = {}
    for name, ident, parsed in runs:
        if ident["extra"]["kind"] != "kern" or not ident["extra"]["expiry"]:
            continue
        for rec in parsed["expiry"]:
            key = (rec["shape"], rec["ctx"], rec["kind"])
            edata.setdefault(key, []).append(rec)
    for (shape, ctx, kind), recs in sorted(edata.items()):
        if kind == "exp_allow":
            continue
        report["kern_expiry"][f"{shape}/{ctx}/{kind}"] = {
            "budget": recs[0].get("budget"),
            "totals": summarize([x for r in recs for x in r["raw"]]),
            "per_boot_mean": [r["stats"]["mean"] for r in recs],
            "allow_mean_per_boot": [r.get("allow_mean") for r in recs],
            "load_cold_per_boot": [r.get("load_cold_ns") for r in recs],
        }
    out = Path(args.runs) / "summary.json"
    out.write_text(json.dumps(report, indent=2) + "\n")
    print("wrote", out)
    for key, entry in report["sys"].items():
        s = entry["samples"]["pooled"]
        print(f"sys {key}: mean {s['mean']:.0f}ns p50 {s['p50']:.0f} p95 {s['p95']:.0f} "
              f"p99 {s['p99']:.0f} max {s['max']:.0f} "
              f"thr {entry['throughput']['pooled']['mean']:.0f}ns/op")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("setup")
    p.add_argument("--kernels", help="comma list, default b0,b1,b2")
    p.set_defaults(func=cmd_setup)

    p = sub.add_parser("build")
    p.add_argument("--kernels")
    p.add_argument("-j", "--jobs", default=os.cpu_count())
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("sys")
    p.add_argument("--runs", default=str(BUILD / "runs/pilot-sys"))
    p.add_argument("--kernels")
    p.add_argument("--scenarios")
    p.add_argument("--timeout", action="store_true", help="add the B2 timeout scenario")
    p.add_argument("--rounds", type=int, default=1)
    p.add_argument("--counts", default=COUNTS_FORMAL)
    p.add_argument("--counts-timeout", default=COUNTS_FORMAL_TIMEOUT)
    p.add_argument("--timeout-s", default="600")
    p.add_argument("--pin", default="8-15")
    p.add_argument("--busybox", default=shutil.which("busybox"))
    p.set_defaults(func=cmd_sys)

    p = sub.add_parser("kern")
    p.add_argument("--runs", default=str(BUILD / "runs/pilot-kern"))
    p.add_argument("--kernels")
    p.add_argument("--expiry", action="store_true")
    p.add_argument("--rounds", type=int, default=1)
    p.add_argument("--timeout-s", default="900")
    p.add_argument("--pin", default="8-15")
    p.add_argument("--busybox", default=shutil.which("busybox"))
    p.set_defaults(func=cmd_kern)

    p = sub.add_parser("parse")
    p.add_argument("--runs", required=True)
    p.set_defaults(func=cmd_parse)

    p = sub.add_parser("stats")
    p.add_argument("--runs", required=True)
    p.set_defaults(func=cmd_stats)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
