#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
out=${1:?usage: build.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cp "$src/tools/testing/selftests/lua_lsm/config" "$out/.config"
make -C "$src" O="$out" olddefconfig
make -C "$src" O="$out" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" bzImage
printf '%s\n' "HOSTCFLAGS=${HOSTCFLAGS:-}" "CC=${CC:-gcc}" "HOSTCC=${HOSTCC:-gcc}" > "$out/build-environment"
# Record exactly the tracked and new test/production inputs, plus build outputs.
git -C "$src" rev-parse HEAD > "$out/source-commit"
git -C "$src" diff HEAD -- lib/lua security/lua lib/Kconfig > "$out/source.diff"
(cd "$src"; find lib/lua security/lua include/linux -type f -print0 | sort -z | xargs -0 sha256sum) > "$out/source-sha256"
sha256sum "$out/.config" "$out/arch/x86/boot/bzImage" > "$out/build-sha256"
