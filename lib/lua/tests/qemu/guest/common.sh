#!/bin/sh

export PATH=/bin

# One printk record prevents concurrent kernel output splitting a case marker.
log()
{
	if test -c /dev/kmsg; then
		printf '<6>%s\n' "$*" > /dev/kmsg
	else
		printf '%s\n' "$*"
	fi
}

fail()
{
	log "LUA_LSM_SMOKE: FAIL: $*"
	sync
	reboot -f
	sleep 2
	exec sh
}

log 'LUA_LSM_SMOKE: booted initramfs'
uname -a

mount -t devtmpfs devtmpfs /dev || fail 'cannot mount devtmpfs'
mount -t proc proc /proc || fail 'cannot mount procfs'
mount -t sysfs sysfs /sys || fail 'cannot mount sysfs'
mkdir -p /sys/kernel/security
mount -t securityfs securityfs /sys/kernel/security || fail 'cannot mount securityfs'

active_lsms=$(cat /sys/kernel/security/lsm) || fail 'cannot read active LSMs'
log "LUA_LSM_SMOKE: active LSMs: $active_lsms"
case ",$active_lsms," in
	*,lua,*) ;;
	*) fail 'lua is not in the active LSM list' ;;
esac

test -r /sys/kernel/security/lua/version || fail 'Lua-LSM securityfs files are missing'
log "LUA_LSM_SMOKE: version $(cat /sys/kernel/security/lua/version)"

echo 'secret' > /lua-lsm-denied || fail 'cannot create policy target'
cat /lua-lsm-smoke.lua > /sys/kernel/security/lua/register || fail 'cannot register smoke policy'
grep -q '^smoke ' /sys/kernel/security/lua/modules || fail 'registered module not listed'
test -r /sys/kernel/security/lua/stats || fail 'stats file is missing'
test -r /sys/kernel/security/lua/lsm_funcs || fail 'hook stats file is missing'

if cat /lua-lsm-denied >/dev/null 2>&1; then
	fail 'file_open policy did not deny the protected path'
fi
log 'LUA_LSM_SMOKE: policy denial observed'

unregister_attempt=1
while ! echo smoke > /sys/kernel/security/lua/unregister; do
	if [ "$unregister_attempt" -ge 10 ]; then
		fail 'cannot unregister smoke policy after 10 attempts'
	fi
	log "LUA_LSM_SMOKE: unregister busy, retrying ($unregister_attempt/10)"
	unregister_attempt=$((unregister_attempt + 1))
	sleep 1
done
if ! cat /lua-lsm-denied >/dev/null; then
	fail 'access did not recover after policy unload'
fi

log 'LUA_LSM_SMOKE: unload recovery observed'
log 'LUA_LSM_SMOKE: PASS'


unload_policy()
{
	attempt=1
	while ! echo "$1" > /sys/kernel/security/lua/unregister; do
		test "$attempt" -lt 10 || fail "unregister $1"
		attempt=$((attempt + 1))
		sleep 1
	done
	cat "$2" >/dev/null || fail "$1 unload recovery"
}
