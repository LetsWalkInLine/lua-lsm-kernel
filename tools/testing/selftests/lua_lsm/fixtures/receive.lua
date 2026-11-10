local kernel=require('kernel')
return {name='receive',author='Lua-LSM tests',license='GPL-2.0',version=1,
 description='UDP receive timeout and recovery',
 socket_sock_rcv_skb=function(sk,skb)
  assert(sk:is_udp())
  kernel.printk('LUA_RX_POLICY: timeout entered')
  pcall(function()while true do end end)
  return true
 end}
