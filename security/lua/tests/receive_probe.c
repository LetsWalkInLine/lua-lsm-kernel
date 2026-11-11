// SPDX-License-Identifier: GPL-2.0-only
/* Selftest observer: no changes to the production dispatcher or Lua API. */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kprobes.h>

static int receive_enter(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(bool *)ri->data = in_serving_softirq();
	return 0;
}

static int receive_return(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	pr_info("LUA_RX_OBSERVER: softirq=%d ret=%d\n", *(bool *)ri->data,
		(int)regs_return_value(regs));
	return 0;
}

static struct kretprobe receive_probe = {
	.kp.symbol_name = "lua_lsm_socket_sock_rcv_skb",
	.entry_handler = receive_enter,
	.handler = receive_return,
	.data_size = sizeof(bool),
	.maxactive = 64,
};

static int __init receive_probe_init(void)
{
	return register_kretprobe(&receive_probe);
}
late_initcall(receive_probe_init);
