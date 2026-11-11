-- SPDX-License-Identifier: GPL-2.0-only
if current and current:comm()=='dispatch-probe' then error('lazy-load failure') end
return {name='dispatch_lazy',author='Lua-LSM tests',description='Lazy load errors',
 license='GPL-2.0',version=1,file_open=function()return true end}
