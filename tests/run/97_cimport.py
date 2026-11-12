# cimport 调用系统 C 库：常量内联、double/整数/指针/字符串返回值。
cimport clib as c
cimport clib

print(c.PI)
print(c.ANSWER)
print(c.SMALL)
print(c.CODE)
print(c.NAME)
print(c.FLAG)
print(c.OFF)
print(c.sqrt(9.0))
print(c.pow(2.0, 10.0))
print(c.pow(y=2.0, x=3.0))
print(c.strlen("hello"))
print(c.labs(-1234567890123))
print(c.abs32(-7))
print(c.is_alpha(65))
print(c.is_alpha(48))

p = c.malloc(8)
print(p is not None)
q = c.memset(p, 0, 8)
print(q is not None)
c.free(p)
c.free_opt(None)

print(c.getenv("PATH") is not None)
print(c.getenv("PYADAMAS_NOT_SET_XYZ") is None)
print(clib.sqrt(16.0))
# syscall(getpid) 有 8 个参数，后两个走栈。
print(c.syscall(39, 0, 0, 0, 0, 0, 0, 0) > 0)

# --- expect ---
# 3.141592653589793
# 42
# 7
# 8
# pyadamas
# True
# False
# 3.0
# 1024.0
# 9.0
# 5
# 1234567890123
# 7
# True
# False
# True
# True
# True
# True
# 4.0
# True
