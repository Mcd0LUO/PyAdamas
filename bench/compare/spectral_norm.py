# spectral_norm —— 谱范数迭代（Benchmarks Game 经典浮点基准）
N = 600


def eval_a(i, j):
    return 1.0 / (((i + j) * (i + j + 1) // 2) + i + 1)


def mul_ata(v, out, n):
    for i in range(n):
        total = 0.0
        for j in range(n):
            total += eval_a(i, j) * v[j]
        out[i] = total


def mul_a(u, out, n):
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
