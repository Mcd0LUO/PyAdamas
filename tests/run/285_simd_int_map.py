# SIMD int map 向量化回归（0.334，PYADAMAS_SIMD）：matmul 形态 c[base+j] += aik*b[base2+j]。
# 守卫范围内（aik∈[0,2^31)、b/c 非负小值）走向量路径：N=7 奇数尾 + N=4 偶数吃尽 + N=1。
# pmuludq 组合乘 + paddq 与标量 imulq/addq 逐位一致；输出对拍 CPython。
def mm(a: list[int], b: list[int], c: list[int], n: int) -> None:
    for i in range(n):
        for k in range(n):
            aik = a[i * n + k]
            if aik != 0:
                for j in range(n):
                    c[i * n + j] += aik * b[k * n + j]

def mk(n: int, seed: int) -> list[int]:
    out = []
    for i in range(n * n):
        out.append((i * seed + 3) % 11)
    return out

def total(c: list[int]) -> int:
    t = 0
    for v in c:
        t += v
    return t

for n in (7, 4, 1):
    a = mk(n, 7)
    b = mk(n, 5)
    c = []
    for _ in range(n * n):
        c.append(0)
    mm(a, b, c, n)
    print(n, total(c))

# --- expect ---
# 7 8694
# 4 1584
# 1 9
