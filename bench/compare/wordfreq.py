# wordfreq —— LCG 生成 N 个词并用 dict 计数（dict / str 路径）
N = 200000

x = 12345
counts: dict[str, int] = {}
i = 0
while i < N:
    x = (x * 1103515245 + 12345) % 2147483648
    w = "w" + str(x % 1000)
    if w in counts:
        counts[w] = counts[w] + 1
    else:
        counts[w] = 1
    i = i + 1

total = 0
best = 0
for k in counts:
    v = counts[k]
    total = total + v
    if v > best:
        best = v
print(len(counts), total, best)
