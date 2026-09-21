# Finite Lua-LSM QEMU regression

`run.py` packages the existing finite smoke, depth, work, deep-error and
66th-frame policies and runs them in an x86_64 KVM guest. The guest init files
and Lua policies are byte-for-byte copies of the M4-03 inputs used again by
M5-03/M5-04. This runner replaces workspace-specific host paths; it does not
change their assertions. It has no Kbuild entry.

Requirements: Python 3, GNU cpio and timeout, QEMU/KVM, an accessible `/dev/kvm`,
a **statically linked x86_64 BusyBox** with the normal shell/mount/cat/grep/reboot
applets, and a separately built test kernel. Do not run these policies on the
host. QEMU attaches no disks or network devices and always uses a 300-second
host watchdog with a 10-second forced termination grace period.

From the kernel source root, with a new output directory for each invocation:

```sh
python3 lib/lua/tests/qemu/run.py --mode regression \
  --kernel /absolute/build/arch/x86/boot/bzImage \
  --busybox /absolute/path/to/static-busybox --output /tmp/lua-regression-1
python3 lib/lua/tests/qemu/run.py --mode entry66 \
  --kernel /absolute/build/arch/x86/boot/bzImage \
  --busybox /absolute/path/to/static-busybox --output /tmp/lua-entry66-1
```

The normal mode requires the current 39 KUnit cases (18 + 21), all LSM case
markers, smoke/unload recovery and a clean exit. Entry66 disables KUnit and
requires caught/reuse, current uncaught-error allow behavior and unload recovery.
Neither the guest's early smoke PASS marker nor an exit code alone is sufficient.
The runner rejects kernel/sanitizer warnings, failed cases and missing markers.
It saves console output, exit status, actual argv and kernel/initramfs/BusyBox
SHA-256 identities in the output directory; an existing directory is rejected.

For packaging or offline checks without booting:

```sh
python3 lib/lua/tests/qemu/run.py --mode regression --prepare-only \
  --busybox /absolute/path/to/static-busybox --output /tmp/lua-package-1
python3 lib/lua/tests/qemu/run.py --mode regression --check-log /path/to/console.log
```

`--check-log` validates log content only, not kernel identity or process exit
status. Runtime validation additionally checks QEMU/watchdog exit status. The
script intentionally does not provide GDB sampling or performance measurement;
those use the archived experimental scripts and their recorded environments.
See [VALIDATION.md](../VALIDATION.md) for build instructions and evidence.
