# stdlib.linalg：BLAS 封装，用户只传数据与维度。
import linalg

c = linalg.matmul([1.0, 2.0, 3.0, 4.0], [5.0, 6.0, 7.0, 8.0], 2, 2, 2)
print(c[0])
print(c[1])
print(c[2])
print(c[3])

# 2x3 * 3x2
d = linalg.matmul([1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
                  [7.0, 8.0, 9.0, 10.0, 11.0, 12.0], 2, 2, 3)
print(d[0])
print(d[1])
print(d[2])
print(d[3])

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
# 32.0
