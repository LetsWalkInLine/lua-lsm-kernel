#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Host-side kselftest; never run dangerous policies on the host kernel.
set -u
cd "$(dirname "$0")" || exit 1
printf 'TAP version 13\n'
if test -z "${LUA_LSM_KERNEL:-}" || test -z "${LUA_LSM_BUSYBOX:-}"; then
 echo '1..0 # SKIP set LUA_LSM_KERNEL and LUA_LSM_BUSYBOX for watchdog QEMU guests'
 exit 4
fi
for tool in python3 qemu-system-x86_64 timeout cc cpio ldd ip lua5.1; do
 if ! command -v "$tool" >/dev/null; then
  echo "1..0 # SKIP missing $tool"
  exit 4
 fi
done
out=${LUA_LSM_OUTPUT:-$(mktemp -d /tmp/lua-lsm-selftest.XXXXXX)}
mkdir -p "$out" || exit 1
echo '1..6'
i=0
failed=0
for mode in kunit dispatch budget strings dump32 network; do
 i=$((i+1))
 set --
 if test -n "${LUA_LSM_CONFIG:-}"; then set -- --config "$LUA_LSM_CONFIG"; fi
 if ./run-qemu.py "$@" --mode "$mode" --kernel "$LUA_LSM_KERNEL" --busybox "$LUA_LSM_BUSYBOX" --output "$out/$mode" >"$out/$mode.runner.log" 2>&1; then
  echo "ok $i $mode"
 else
  echo "not ok $i $mode # see $out/$mode.runner.log"
  failed=1
 fi
done
exit "$failed"
