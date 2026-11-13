"""分代 GC 基准：大存活堆 + 大量短命临时容器，夹杂少量环。"""
keep = []
i = 0
while i < 100000:
    keep.append("live-" + str(i))
    i += 1

total = 0
i = 0
while i < 150000:
    tmp = ["x" * 16, str(i), i * 2]
    total = total + len(tmp)
    if i % 50 == 0:
        a = []
        b = [a, i]
        a.append(b)
    i += 1
print(total)
