# os.uname（0.520）：5 个字符串属性。与 CPython 3.14.4 逐字节一致。
# sysname/machine 是宿主值，两边读同一个 uname(2)，所以可以直接打印。
import os

u = os.uname()
print(u.sysname)
print(u.machine)
print(u.nodename != '')
print(u.release != '')
print(u.version != '')

# --- expect ---
# Linux
# x86_64
# True
# True
# True
