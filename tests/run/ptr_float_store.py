# 0.542：ptr[float] 的**变量下标**写入（编组：f64 走 xmm0）。
# 注意：malloc_f(n) 的参数是**字节数**（映射到 C 的 malloc），5 个 double 要 40。
cimport clib

p = clib.malloc_f(40)
p[0] = 1.0
p[4] = 4.0
print(p[0], p[4])

i: int = 0
while i < 5:
    p[i] = i * 1.0
    i = i + 1
print(p[0], p[4])

j: int = 0
for j in range(0, 5):
    p[j] = 10.0 + j
print(p[0], p[4])
clib.free(p)

q = clib.malloc_i(40)
k: int = 0
while k < 5:
    q[k] = k * 3
    k = k + 1
print(q[0], q[4])
clib.free(q)

# --- expect ---
# 1.0 4.0
# 0.0 4.0
# 10.0 14.0
# 0 12
