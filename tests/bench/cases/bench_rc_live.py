"""RC/GC 基准：大存活堆 + 大量短命临时值。"""
keep: list[str] = []
i = 0
while i < 3000:
    keep.append("item-" + str(i))
    i += 1

total = 0
j = 0
while j < 20000:
    tmp = "tmp-" + str(j) + "-payload"
    total += len(tmp)
    j += 1
print(len(keep), total)
