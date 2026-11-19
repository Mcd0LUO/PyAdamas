# u2_hetero_dict —— unknown 冷路径：异构 dict 的 float 值写入/读取/算术
N = 120000

d: dict[str, object] = {}
i: int = 0
while i < N:
    d["f" + str(i)] = 1.25
    d["i" + str(i)] = i
    i = i + 1

total: object = 0.0
i = 0
while i < N:
    k: str = "f" + str(i)
    total = total + d[k]
    i = i + 1
print(len(d))
print(total)
