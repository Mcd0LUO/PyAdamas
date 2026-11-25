# 0.536：buffer 作为 ptr[float] 实参传给 C（memset / memcpy），调用后 buffer 仍有效。
cimport cbuflib as c

b = buffer(4)
b[0] = 1.5
b[1] = 2.5
print(b[0], b[1])

# memset 清零：4 个 double = 32 字节
c.memset(b, 0, 32)
print(b[0], b[1], b[2])

# 拷贝到另一个 buffer，两个都还能读
b[0] = 7.5
b[1] = 8.5
d = buffer(4)
c.memcpy(d, b, 32)
print(d[0], d[1])
print(b[0], b[1])

# --- expect ---
# 1.5 2.5
# 0.0 0.0 0.0
# 7.5 8.5
# 7.5 8.5
