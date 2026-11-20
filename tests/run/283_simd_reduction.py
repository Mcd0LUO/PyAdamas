# SIMD reduction 向量化回归（0.333，PYADAMAS_SIMD）：spectral_norm 形态 N=5（奇数）
# 钉住 eval_A 乘加累加 reduction 输出。保序累加 → 与标量逐位一致，flag-on/off
# 输出相同且对拍 CPython。mul_ata(a_is_j=0)+mul_a(a_is_j=1) 两条向量路径 + 奇数尾循环。
N = 5
def eval_a(i: int, j: int) -> float:
    return 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1)
def mul_ata(v: list[float], out: list[float], n: int) -> None:
    for i in range(n):
        total = 0.0
        for j in range(n):
            total += eval_a(i, j) * v[j]
        out[i] = total
def mul_a(u: list[float], out: list[float], n: int) -> None:
    for i in range(n):
        total = 0.0
        for j in range(n):
            total += eval_a(j, i) * u[j]
        out[i] = total
u = []
v = []
tmp = []
for _ in range(N):
    u.append(1.0)
    v.append(0.0)
    tmp.append(0.0)
for _ in range(10):
    mul_ata(u, v, N)
    mul_a(v, tmp, N)
v_bv = 0.0
v_vv = 0.0
for i in range(N):
    v_bv += u[i] * tmp[i]
    v_vv += tmp[i] * tmp[i]
print(int((v_bv / v_vv) ** 0.5 * 1000000.0))

# --- expect ---
# 794176
