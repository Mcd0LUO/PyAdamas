# 真实 OpenBLAS：list[float] 直接作为 double* 传给 cblas。
cimport cblas

# 2x2 行主序：[[1,2],[3,4]] * [[5,6],[7,8]] = [[19,22],[43,50]]
a: list[float] = [1.0, 2.0, 3.0, 4.0]
b: list[float] = [5.0, 6.0, 7.0, 8.0]
c: list[float] = [0.0, 0.0, 0.0, 0.0]
cblas.dgemm(cblas.ROW_MAJOR, cblas.NO_TRANS, cblas.NO_TRANS,
            2, 2, 2, 1.0, a, 2, b, 2, 0.0, c, 2)
print(c[0])
print(c[1])
print(c[2])
print(c[3])

# 点积：[1,2,3] · [4,5,6] = 32
x: list[float] = [1.0, 2.0, 3.0]
y: list[float] = [4.0, 5.0, 6.0]
print(cblas.ddot(3, x, 1, y, 1))

# --- expect ---
# 19.0
# 22.0
# 43.0
# 50.0
# 32.0
