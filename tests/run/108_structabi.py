# SysV 聚合 ABI：16 字节双 SSE 结构体、24 字节 MEMORY 结构体。
cimport cstructabi as c

p = pair(1.5, 2.5)
q = pair(3.0, 4.0)
r = c.pair_add(p, q)
print(r.a)
print(r.b)

t = triple(1, 2, 3)
u = c.triple_sum(t)
print(u.a)
print(u.b)
print(u.c)

print(c.triple_mix(10, t, 20))

# extern 变量：int / uint32 / float / bool 的读取与写回
print(c.cvar_i)
c.cvar_i = 42
print(c.cvar_i)
print(c.cvar_u)
print(c.cvar_f)
c.cvar_f = 1.25
print(c.cvar_f)
print(c.cvar_b)
c.cvar_b = False
print(c.cvar_b)

# 4 字节指针解引用：int32 符号扩展、uint32 零扩展
pi = c.ptr_i32()
print(pi[0])
print(pi[1])
pi[0] = 9
print(pi[0])
pu = c.ptr_u32()
print(pu[0])
print(pu[1])

# --- expect ---
# 4.5
# 6.5
# 2
# 4
# 6
# 36
# 1234567890123
# 42
# 4294967295
# 2.5
# 1.25
# True
# False
# -5
# 7
# 9
# 4000000000
# 1
