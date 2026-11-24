"""pprint 基准：嵌套 dict/list 的 pformat（键排序 + 断行排版）。"""
import pprint

data = {}
i = 0
while i < 40:
    data["key" + str(i)] = [i, i * 2, {"n": i, "s": "v" + str(i), "f": i * 1.5}]
    i += 1

total = 0
k = 0
while k < 600:
    total += len(pprint.pformat(data))
    k += 1
print(total)
