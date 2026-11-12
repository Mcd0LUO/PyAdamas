# stdlib.linalg：BLAS 封装，用户只传数据与维度。
import linalg

a: list[float] = [1.0, 2.0, 3.0, 4.0]
b: list[float] = [5.0, 6.0, 7.0, 8.0]
c = linalg.matmul(a, b, 2, 2, 2)
print(c[0])
print(c[1])
print(c[2])
print(c[3])

# 2x3 * 3x2
a2: list[float] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
b2: list[float] = [7.0, 8.0, 9.0, 10.0, 11.0, 12.0]
d = linalg.matmul(a2, b2, 2, 2, 3)
print(d[0])
print(d[1])
print(d[2])
print(d[3])

# A 以 3x2 转置形式存储：op(A) = [[1,2,3],[4,5,6]]
at: list[float] = [1.0, 4.0, 2.0, 5.0, 3.0, 6.0]
e = linalg.matmul(at, b2, 2, 2, 3, trans_a=True)
print(e[0])
print(e[1])
print(e[2])
print(e[3])

# B 以 2x3 转置形式存储：op(B) = [[7,8],[9,10],[11,12]]
bt: list[float] = [7.0, 9.0, 11.0, 8.0, 10.0, 12.0]
f = linalg.matmul(a2, bt, 2, 2, 3, trans_b=True)
print(f[0])
print(f[1])
print(f[2])
print(f[3])

# 矩阵-向量：2x3 * [1,1,1]
v = linalg.matvec(a2, [1.0, 1.0, 1.0], 2, 3)
print(v[0])
print(v[1])

# 转置：op(A) 为 3x2，x 长度 2
w = linalg.matvec(a2, [1.0, 1.0], 2, 3, trans_a=True)
print(w[0])
print(w[1])
print(w[2])

# 复用输出缓冲
out: list[float] = [0.0, 0.0, 0.0, 0.0]
linalg.matmul_into(out, a, b, 2, 2, 2)
print(out[0])
print(out[1])
print(out[2])
print(out[3])

print(linalg.dot([1.0, 2.0, 3.0], [4.0, 5.0, 6.0]))

# --- expect ---
# 19.0
# 22.0
# 43.0
# 50.0
# 58.0
# 64.0
# 139.0
# 154.0
# 58.0
# 64.0
# 139.0
# 154.0
# 58.0
# 64.0
# 139.0
# 154.0
# 6.0
# 15.0
# 5.0
# 7.0
# 9.0
# 19.0
# 22.0
# 43.0
# 50.0
# 32.0
