local kernel=require('kernel')
return {name='receive_allow',author='Lua-LSM tests',license='GPL-2.0',version=1,
 description='UDP receive allow',
 socket_sock_rcv_skb=function(sk,skb)
  assert(sk:is_udp())
  kernel.printk('LUA_RX_POLICY: allowed')
  return true
 end}
