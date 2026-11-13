# 指针解引用：ptr[int] / ptr[float] 的元素读写。
cimport clib as c

p = c.malloc_i(24)
p[0] = 42
p[1] = -7
p[2] = 100
print(p[0])
print(p[1])
print(p[2])
c.free(p)

q = c.malloc_f(16)
q[0] = 1.5
q[1] = 2.25
print(q[0])
print(q[1])
c.free(q)

# --- expect ---
# 42
# -7
# 100
# 1.5
# 2.25
