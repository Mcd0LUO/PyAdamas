# 缓冲区传参：list/bytes 的连续 data 直接交给 C。
cimport clib

# 输出参数：modf 把整数部分写进 list[float] 的 data。
buf: list[float] = [0.0]
frac = clib.modf(3.75, buf)
print(frac)
print(buf[0])

# list[int] 的槽位是 8 字节，time 按 time_t* 写回。
stamp: list[int] = [0]
now = clib.time_at(stamp)
print(stamp[0] > 0)
print(stamp[0] == now)

# 无类型 ptr 同时接受 list[float] 与 bytes。
a: list[float] = [1.5, 2.5]
b: list[float] = [1.5, 2.5]
print(clib.memcmp(a, b, 16) == 0)
print(clib.memcmp(b"abc", b"abd", 3) != 0)

# --- expect ---
# 0.75
# 3.0
# True
# True
# True
# True
