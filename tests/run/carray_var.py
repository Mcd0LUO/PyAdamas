# 0.563：array[T, N] 的 T 是元素宽度、N 是元素个数；下标读写按 T 的宽度进行。
cimport carrayvar as c

print(c.vals_addr() == c.vals3)

p = c.vals3
if p is not None:
    print(p[0], p[1], p[2])
    p[1] = 99
    print(p[1])

q = c.vals64
if q is not None:
    print(q[0], q[1], q[2])

# --- expect ---
# True
# 10 20 30
# 99
# 100 200 300
