# pyadamas: vect
# 457: 手写 while 循环的向量化（0.387 P2）。
# `while i < n: …; i += 1` 降成 cond/body 两块、增量在体块尾部，跟 for 形状
# 不同，以前整段被跳过。现在两条路径等价：while 与 for 的向量化结果一致。
# 覆盖：归约、map（原地写）、n=0/1/2/3/5 边界、负值、不可向量化的 fallback
# （循环体里有函数调用 → 保守标量，输出仍须正确）。


def reduce_while(n: int) -> float:
    a: list[float] = []
    i = 0
    while i < n:
        a.append(float(i) * 0.5 - 1.0)
        i = i + 1
    i = 0
    s = 0.0
    while i < n:
        s += a[i] * a[i] + 0.25
        i = i + 1
    return s


def map_while(n: int) -> None:
    a: list[float] = []
    out: list[float] = []
    i = 0
    while i < n:
        a.append(float(i))
        out.append(0.0)
        i = i + 1
    i = 0
    while i < n:
        out[i] = a[i] * 2.0 + 1.0
        i = i + 1
    print(format(out[0], ".1f"), format(out[n - 1], ".1f"))


def with_call(n: int) -> float:
    # 循环体里有函数调用 → 向量化必须保守跳过，走标量
    def f(x: float) -> float:
        return x + 1.0

    a: list[float] = [2.0] * n
    i = 0
    s = 0.0
    while i < n:
        s += f(a[i])
        i = i + 1
    return s


for n in [0, 1, 2, 3, 5, 64]:
    print(n, format(reduce_while(n), ".4f"))
    if n > 0:
        map_while(n)
    print("call", format(with_call(n), ".1f"))

# 与等价的 for 写法逐位对齐
def reduce_for(n: int) -> float:
    a: list[float] = []
    i = 0
    while i < n:
        a.append(float(i) * 0.5 - 1.0)
        i = i + 1
    s = 0.0
    for i in range(n):
        s += a[i] * a[i] + 0.25
    return s


for n in [0, 1, 7, 128, 1000]:
    print("eq", n, reduce_while(n) == reduce_for(n))
# --- expect ---
# 0 0.0000
# call 0.0
# 1 1.2500
# 1.0 1.0
# call 3.0
# 2 1.7500
# 1.0 3.0
# call 6.0
# 3 2.0000
# 1.0 5.0
# call 9.0
# 5 3.7500
# 1.0 9.0
# call 15.0
# 64 19400.0000
# 1.0 127.0
# call 192.0
# eq 0 True
# eq 1 True
# eq 7 True
# eq 128 True
# eq 1000 True
