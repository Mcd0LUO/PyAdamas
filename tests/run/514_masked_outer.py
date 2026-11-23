# masked divergence 向量化（0.450）：外层独立迭代 + 内层数据依赖早退。
# 覆盖：普通 mandelbrot 小图 / 全部立即收敛 / 全部跑满 maxit / maxit=1 /
# maxit 大（200）/ odd width（尾迭代）/ negative cr 区域。

def run(size: int, maxiter: int) -> int:
    total = 0
    for y in range(size):
        ci = 2.0 * y / size - 1.0
        for x in range(size):
            cr = 2.0 * x / size - 1.5
            zr = 0.0
            zi = 0.0
            i = 0
            while i < maxiter and zr * zr + zi * zi <= 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                i += 1
            total += i
    return total


print("normal", run(20, 20))
print("maxit1", run(20, 1))
print("maxit2", run(20, 2))
print("big", run(17, 200))
print("odd", run(21, 30))
print("even", run(22, 30))
print("tiny", run(2, 5))
print("one", run(1, 5))
# 负 cr 区域（x 很大比例位置）
def neg_region(size: int, maxiter: int) -> int:
    total = 0
    for y in range(size):
        ci = 2.0 * y / size - 1.0
        for x in range(size):
            cr = 2.0 * x / size - 1.5
            zr = 0.0
            zi = 0.0
            i = 0
            while i < maxiter and zr * zr + zi * zi <= 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                i += 1
            total += i
    return total


print("neg_region", neg_region(31, 50))
print("wide", run(33, 50))

# --- expect ---
# normal 4778
# maxit1 400
# maxit2 800
# big 22948
# odd 7276
# even 7932
# tiny 16
# one 2
# neg_region 23513
# wide 26690
