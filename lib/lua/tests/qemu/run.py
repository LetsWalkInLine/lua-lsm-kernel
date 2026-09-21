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


def check_log(log, mode):
    if re.search(r"BUG:|WARNING:|Oops:|Kernel panic|KASAN:|UBSAN:|FAIL:|"
                 r"(?:^|\]\s*|\s)not ok \d", log, re.M):
        raise ValueError("guest failure or diagnostic in log")
    required = ["LUA_LSM_SMOKE: PASS"]
    if mode == "entry66":
        required += ["M4_ENTER66: PASS caught", "M4_ENTER66: ALLOW uncaught",
                     "M4_ENTER66: PASS\n"]
    else:
        required += ["lua-string: pass:18 fail:0 skip:0 total:18",
                     "lua-string-work: pass:21 fail:0 skip:0 total:21",
                     "M2_DEPTH: PASS\n", "M4_WORK: PASS\n", "M4_DEEP: PASS\n",
                     "pattern recursion limit exceeded"]
        for prefix, count in [("M2_DEPTH: PASS ", 9), ("M4_WORK: PASS ", 6),
                              ("M4_DEEP: PASS ", 10), ("M4_DEEP: ALLOW ", 4)]:
            if len(re.findall(r"^" + prefix + r"\S+", log, re.M)) != count:
                raise ValueError("incomplete case markers: " + prefix)
    required += ["string work limit exceeded"]
    for marker in required:
        if marker not in log:
            raise ValueError("missing marker: " + marker)


def make_image(out, mode, busybox):
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
    guest = Path(__file__).resolve().parent / "guest"
    shutil.copyfile(guest / ("init-" + mode), root / "init")
    (root / "init").chmod(0o755)
    for policy in guest.glob("*.lua"):
        shutil.copyfile(policy, root / policy.name)
    names = ["."] + [str(p.relative_to(root)) for p in sorted(root.rglob("*"))]
    archive = subprocess.run(["cpio", "--null", "-o", "--format=newc", "--owner=0:0"],
                             cwd=root, input=("\0".join(names) + "\0").encode(),
                             stdout=subprocess.PIPE, check=True).stdout
    image = out / "initramfs.cpio.gz"
    image.write_bytes(gzip.compress(archive, mtime=0))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["regression", "entry66"], required=True)
    parser.add_argument("--check-log", type=Path, help="validate an existing log only")
    parser.add_argument("--output", type=Path, help="new directory; must not exist")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--busybox", type=Path, help="static x86_64 BusyBox executable")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
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
    image = make_image(out, args.mode, busybox)
    identity = {"mode": args.mode, "busybox_sha256": digest(busybox),
                "initramfs_sha256": digest(image)}
    if not args.prepare_only:
        append = ("console=ttyS0,115200 rdinit=/init panic=-1 oops=panic "
                  "panic_on_warn=1 kasan.fault=panic lsm=lua nokaslr")
        if args.mode == "entry66":
            append += " kunit.enable=0"
        argv = ["timeout", "--foreground", "--kill-after=10s", "300s",
                "qemu-system-x86_64", "-machine", "pc,accel=kvm", "-cpu", "qemu64",
                "-smp", "2", "-m", "2048", "-nodefaults", "-no-reboot",
                "-display", "none", "-monitor", "none", "-serial", "stdio",
                "-kernel", str(kernel), "-initrd", str(image), "-append", append]
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
