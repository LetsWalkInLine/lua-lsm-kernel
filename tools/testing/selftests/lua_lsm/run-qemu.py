#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package and run the finite x86_64 Lua-LSM regression guest."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_commit():
    """Return source-tree metadata when available; installed tests need none."""
    here = Path(__file__).resolve().parent
    for source in here.parents:
        if source / "tools/testing/selftests/lua_lsm" != here:
            continue
        if not (source / ".git").exists():
            continue
        try:
            git = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                                 capture_output=True, text=True)
        except OSError:
            return None
        return git.stdout.strip() if git.returncode == 0 else None
    return None


def check_log(log, mode):
    log = re.sub(r"^\[\s*\d+\.\d+\]\s*", "", log, flags=re.M)
    if re.search(r"BUG:|WARNING:|Oops:|Kernel panic|KASAN:|UBSAN:|FAIL:|"
                 r"(?:^|\]\s*|\s)not ok \d", log, re.M):
        raise ValueError("guest failure or diagnostic in log")
    required = ["LUA_LSM_SMOKE: PASS"]
    if mode in ("budget", "dispatch"):
        prefix = "LUA_" + mode.upper()
        required += [prefix + ": PASS " + n for n in
                     ("same-task", "other-task", "unload", "lazy", "lazy-unload")]
        required += [prefix + ": PASS\n"]
    elif mode == "strings":
        required += ["LUA_STRINGS: PASS " + n for n in ("work", "depth65-66")]
    elif mode == "dump32":
        required += ["LUA_DUMP32: PASS " + n for n in
                     ("register32", "register33-reject", "source32", "source33-recovery", "unload")]
    elif mode == "network":
        required += ["LUA_RX: PASS " + n for n in ("allow", "timeout", "recovery")]
        required += ["LUA_RX_POLICY: timeout entered", "LUA_RX_POLICY: allowed",
                     "LUA_RX_OBSERVER: softirq=1 ret=-5",
                     "LUA_RX_OBSERVER: softirq=1 ret=0",
                     "hook socket_sock_rcv_skb failed: Lua status 6"]
        # Ensure an observed successful hook after the timed-out receive.
        tail = log.split("LUA_RX: PASS timeout", 1)[-1]
        if "LUA_RX_OBSERVER: softirq=1 ret=0" not in tail:
            raise ValueError("no successful receive hook after timeout")
    elif mode == "kunit":
        for name, count in [("lua_string_limits", 14), ("lua_lsm_dispatch_errors", 8),
                            ("lua_lsm_vm_budget", 10), ("lua_lsm_vm_budget_clock", 2),
                            ("lua_lsm_vm_budget_return", 3)]:
            required.append(f"{name}: pass:{count} fail:0 skip:0 total:{count}")
        for name in ("lua_lsm_getsecurity", "lua_lsm_string_budget_integration"):
            if not re.search(r"\bok \d+ " + name + r"\s*$", log, re.M):
                raise ValueError("missing successful suite " + name)
    if mode in ("strings", "dump32", "network"):
        required.append({"strings": "LUA_STRINGS", "dump32": "LUA_DUMP32", "network": "LUA_RX"}[mode] + ": PASS\n")
    for marker in required:
        if marker not in log:
            raise ValueError("missing marker: " + marker)
        if ": PASS" in marker and len(re.findall(re.escape(marker.rstrip()) + r"\s*$", log, re.M)) != 1:
            raise ValueError("duplicate marker: " + marker)


def copy_executable(binary, root, target):
    shutil.copyfile(binary, root / target)
    (root / target).chmod(0o755)
    result = subprocess.run(["ldd", str(binary)], text=True, capture_output=True)
    # Static executables require no loader. ldd is used only on supplied host tools.
    for name in re.findall(r"(?:=>\s*)?(/[^\s()]+)", result.stdout):
        source = Path(name)
        dest = root / source.relative_to("/")
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)


def make_image(out, mode, busybox, ip):
    # Only construct a new tree. Never replace a caller's existing directory.
    root = out / "rootfs"
    root.mkdir()
    for name in ["bin", "dev", "proc", "sys", "tmp"]:
        (root / name).mkdir()
    (root / "tmp").chmod(0o1777)
    shutil.copyfile(busybox, root / "bin/busybox")
    (root / "bin/busybox").chmod(0o755)
    for applet in subprocess.check_output([str(busybox), "--list"], text=True).split():
        if applet == "busybox":
            continue
        if Path(applet).name != applet:
            raise ValueError("unexpected busybox applet: " + applet)
        (root / "bin" / applet).symlink_to("busybox")
    guest = Path(__file__).resolve().parent / "fixtures"
    shutil.copyfile(guest / ("init-" + mode), root / "init")
    (root / "init").chmod(0o755)
    for policy in [*guest.glob("*.lua"), guest / "common.sh"]:
        shutil.copyfile(policy, root / policy.name)
    if mode in ["dispatch", "budget", "strings", "network", "dump32"]:
        probe = "receive" if mode == "network" else "strings" if mode == "dump32" else mode
        subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(guest / (probe + "-probe.c")), "-o", str(root / (probe + "-probe"))],
                       check=True)
    if mode == "dump32":
        subprocess.run(["python3", str(Path(__file__).with_name("generate-dump.py")), str(root)], check=True)
    if mode == "network":
        (root / "bin/ip").unlink(missing_ok=True)
        copy_executable(ip, root, "bin/ip")
    names = ["."] + [str(p.relative_to(root)) for p in sorted(root.rglob("*"))]
    archive = subprocess.run(["cpio", "--null", "-o", "--format=newc", "--owner=0:0"],
                             cwd=root, input=("\0".join(names) + "\0").encode(),
                             stdout=subprocess.PIPE, check=True).stdout
    image = out / "initramfs.cpio.gz"
    image.write_bytes(gzip.compress(archive, mtime=0))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["kunit", "dispatch", "budget", "strings", "dump32", "network"], required=True)
    parser.add_argument("--check-log", type=Path, help="validate an existing log only")
    parser.add_argument("--output", type=Path, help="new directory; must not exist")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--busybox", type=Path, help="static x86_64 BusyBox executable")
    parser.add_argument("--ip", type=Path, default=shutil.which("ip"), help="iproute2 for network guest")
    parser.add_argument("--config", type=Path, help="exact kernel .config saved with run identity")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if args.mode == "network" and not args.ip:
        parser.error("network mode requires iproute2 --ip")
    if args.check_log:
        check_log(args.check_log.read_text(errors="replace"), args.mode)
        print("Log checks: PASS (does not authenticate the kernel or exit status)")
        return
    if not args.output or not args.busybox or (not args.prepare_only and not args.kernel):
        parser.error("supply --output, --busybox, and --kernel unless --prepare-only")
    out = args.output.resolve()
    busybox = args.busybox.resolve(strict=True)
    kernel = args.kernel.resolve(strict=True) if args.kernel else None
    out.mkdir(parents=True, exist_ok=False)
    image = make_image(out, args.mode, busybox, args.ip)
    identity = {"mode": args.mode, "busybox_sha256": digest(busybox),
                "initramfs_sha256": digest(image)}
    identity["test_sources"] = {str(p.relative_to(Path(__file__).parent)): digest(p)
                                for p in sorted(Path(__file__).parent.rglob("*"))
                                if p.is_file() and "__pycache__" not in str(p)}
    commit = source_commit()
    if commit is not None:
        identity["source_commit"] = commit
    if args.config:
        shutil.copyfile(args.config, out / "kernel.config")
        identity["config_sha256"] = digest(out / "kernel.config")
    if not args.prepare_only:
        append = ("console=ttyS0,115200 rdinit=/init panic=-1 oops=panic "
                  "panic_on_warn=1 kasan.fault=panic lsm=lua nokaslr")
        argv = ["timeout", "--foreground", "--kill-after=10s", "300s",
                "qemu-system-x86_64", "-machine", "pc,accel=tcg", "-cpu", "qemu64",
                "-smp", "2", "-m", "2048", "-nodefaults", "-no-reboot",
                "-display", "none", "-monitor", "none", "-serial", "stdio",
                "-kernel", str(kernel), "-initrd", str(image), "-append", append]
        if args.mode != "kunit":
            argv[-1] += " kunit.enable=0"
        identity.update(kernel_sha256=digest(kernel), argv=argv)
    (out / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    if args.prepare_only:
        print("Prepared", image)
        return
    with (out / "console.log").open("x") as log:
        result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    (out / "exit-status").write_text(str(result.returncode) + "\n")
    if result.returncode:
        raise SystemExit("QEMU/watchdog failed: " + str(result.returncode))
    check_log((out / "console.log").read_text(errors="replace"), args.mode)
    print("Guest checks: PASS; evidence:", out)


if __name__ == "__main__":
    main()
