`$ echo “hello_world” > /dev/faulty`
### The module's attempt to write to a null address
> [   22.813501] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
```
[   22.813949] Mem abort info:
[   22.814053]   ESR = 0x0000000096000044
[   22.814197]   EC = 0x25: DABT (current EL), IL = 32 bits
[   22.814360]   SET = 0, FnV = 0
[   22.814466]   EA = 0, S1PTW = 0
[   22.814574]   FSC = 0x04: level 0 translation fault
[   22.814723] Data abort info:
[   22.814838]   ISV = 0, ISS = 0x00000044, ISS2 = 0x00000000
[   22.815041]   CM = 0, WnR = 1, TnD = 0, TagAccess = 0
[   22.815212]   GCS = 0, Overlay = 0, DirtyBit = 0, Xs = 0
[   22.815442] user pgtable: 4k pages, 48-bit VAs, pgdp=0000000042077000
[   22.815674] [0000000000000000] pgd=0000000000000000, p4d=0000000000000000
[   22.816075] Internal error: Oops: 0000000096000044 [#1] PREEMPT SMP
[   22.816425] Modules linked in: hello(O) faulty(O) scull(O) ipv6
[   22.816941] CPU: 0 PID: 172 Comm: sh Tainted: G           O       6.6.84 #1
[   22.817272] Hardware name: linux,dummy-virt (DT)
[   22.817554] pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
```
### Offending function call
> [   22.817849] pc : faulty_write+0x8/0x10 [faulty]
```
[   22.818273] lr : vfs_write+0xc8/0x30c
[   22.818445] sp : ffff80008027bd20
[   22.818595] x29: ffff80008027bd80 x28: ffff077bc233ad00 x27: 0000000000000000
[   22.818970] x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
[   22.819234] x23: 0000000000000000 x22: ffff80008027bdc0 x21: 0000aaaafcf52180
[   22.819433] x20: ffff077bc2022d00 x19: 0000000000000012 x18: 0000000000000000
[   22.819624] x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
[   22.819825] x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
[   22.820005] x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
[   22.820203] x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
[   22.820383] x5 : 0000000000000000 x4 : ffffc69ec5437000 x3 : ffff80008027bdc0
[   22.820558] x2 : 0000000000000012 x1 : 0000000000000000 x0 : 0000000000000000
[   22.820822] Call trace:
```
### Made it down to memory management before reading a null address
> [   22.821039]  faulty_write+0x8/0x10 [faulty]

> [   22.821243]  ksys_write+0x74/0x10c

> [   22.821348]  __arm64_sys_write+0x1c/0x28

> [   22.821445]  invoke_syscall+0x48/0x118

> [   22.821545]  el0_svc_common.constprop.0+0x40/0xe0

> [   22.821662]  do_el0_svc+0x1c/0x28

> [   22.821748]  el0_svc+0x38/0xcc

> [   22.821829]  el0t_64_sync_handler+0x100/0x12c

> [   22.821938]  el0t_64_sync+0x190/0x194

> [   22.822252] Code: ???????? ???????? d2800001 d2800000 (b900003f) 

> [   22.822519] ---[ end trace 0000000000000000 ]---
