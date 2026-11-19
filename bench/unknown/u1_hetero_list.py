# u1_hetero_list —— unknown 冷路径：异构 list 的 float 装箱 + 读取
# 基线（0.323）里 float 在 list[object] 中存位模式，读取/打印都按整数；
# 0.324 之后 float 装箱成 CPYFLOAT，语义正确，成本只在这条冷路径上。
N = 300000

xs: list[object] = []
i: int = 0
while i < N:
    xs.append(1.5)
    xs.append(i)
    i = i + 1

total: object = 0.0
for v in xs:
    if isinstance(v, float):
        total = total + v
print(total)
