# matmul —— 整数矩阵乘法（连续内存访问 / 嵌套循环）
N = 120


def make(n: int) -> list:
    out = []
    for i in range(n * n):
        out.append((i * 7 + 3) % 11)
    return out


def run() -> int:
    a = make(N)
    b = make(N)
    c = []
    for _ in range(N * N):
        c.append(0)

    for i in range(N):
        for k in range(N):
            aik = a[i * N + k]
            if aik != 0:
                for j in range(N):
                    c[i * N + j] += aik * b[k * N + j]

    total = 0
    for v in c:
        total += v
    return total


print(run())