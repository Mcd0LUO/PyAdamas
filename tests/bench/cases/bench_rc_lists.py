"""RC 基准：短命列表的构造、拼接与释放。"""
total = 0
i = 0
while i < 5000:
    xs = [i, i + 1, i + 2]
    xs.append(i + 3)
    total += len(xs)
    i += 1
print(total)
