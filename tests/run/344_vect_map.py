# 通用循环向量化回归（PYADAMAS_VECT）：非基准浮点数组逐元素 map。
# 任意形状 out[i]=a[i]*c+b[i]（加/乘/不变量），n 覆盖偶/奇/1/0，尾循环。
# flag-on/off 输出相同且对拍 CPython（向量化路径见 PYADAMAS_VECT_DBG）。
def scale_add(a: list[float], b: list[float], out: list[float], n: int, c: float) -> None:
    for i in range(n):
        out[i] = a[i] * c + b[i]

def run(n: int) -> None:
    a = []
    b = []
    out = []
    for i in range(n):
        a.append(float(i) + 0.5)
        b.append(float(i) * 0.25)
        out.append(0.0)
    scale_add(a, b, out, n, 1.5)
    s = 0.0
    for i in range(n):
        s += out[i]
    print(n, s)

run(8)
run(7)
run(1)
run(0)
run(2)
run(3)

# --- expect ---
# 8 55.0
# 7 42.0
# 1 0.75
# 0 0.0
# 2 3.25
# 3 7.5
