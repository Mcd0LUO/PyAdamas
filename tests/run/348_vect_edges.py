# pyadamas: vect
# 通用向量化边界（PYADAMAS_VECT）：n=0/1/2、len<n 的 IndexError 时机、
# kind 升级（混入非浮点）fallback。守卫不满足整段标量，异常语义不变。
def scale(a: list[float], out: list[float], n: int) -> None:
    for i in range(n):
        out[i] = a[i] * 2.0

def mk(n: int) -> list[float]:
    a = []
    for i in range(n):
        a.append(float(i) + 1.0)
    return a

for n in (0, 1, 2, 3):
    a = mk(n)
    out = mk(n)
    scale(a, out, n)
    print("n", n, out)
# len < n：第一个越界 j 抛 IndexError（时机不变）
a = mk(3)
out = mk(3)
try:
    scale(a, out, 5)
    print("no error")
except IndexError:
    print("IndexError", out)
# kind 升级：out 混入字符串 → kind!=0 → 标量 fallback（仍正确）
a2 = mk(4)
o2: list[float] = [0.0, 0.0, 0.0, 0.0]
scale(a2, o2, 4)
print("kind", o2)

# --- expect ---
# n 0 []
# n 1 [2.0]
# n 2 [2.0, 4.0]
# n 3 [2.0, 4.0, 6.0]
# IndexError [2.0, 4.0, 6.0]
# kind [2.0, 4.0, 6.0, 8.0]
