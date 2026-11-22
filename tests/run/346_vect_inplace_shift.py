# pyadamas: vect
# 通用循环向量化回归（PYADAMAS_VECT）：原地 map（读在写前同偏移）+ 偏移下标 map。
# inplace a[i]=a[i]*2+1；shifted out[i]=a[i+1]-a[i]（仿射下标 j+1）。
def inplace(a: list[float], n: int) -> None:
    for i in range(n):
        a[i] = a[i] * 2.0 + 1.0

def shifted(a: list[float], out: list[float], n: int) -> None:
    for i in range(n):
        out[i] = a[i + 1] - a[i]

def mk(n: int) -> list[float]:
    a = []
    for i in range(n):
        a.append(float(i) + 0.5)
    return a

for n in (6, 5, 1, 2):
    a = mk(n)
    inplace(a, n)
    s = 0.0
    for i in range(n):
        s += a[i]
    print("ip", n, s)
for n in (6, 5, 1):
    a = mk(n + 1)
    out = mk(n)
    shifted(a, out, n)
    s = 0.0
    for i in range(n):
        s += out[i]
    print("sh", n, s)

# --- expect ---
# ip 6 42.0
# ip 5 30.0
# ip 1 2.0
# ip 2 6.0
# sh 6 6.0
# sh 5 5.0
# sh 1 1.0
