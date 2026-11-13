"""collections 基准：deque 双端操作 + Counter 计数。"""
from collections import deque, Counter

words = []
for _ in range(100):
    words.append("ab")
    words.append("cd")
    words.append("ab")
    words.append("ef")
    words.append("cd")
    words.append("ab")

total = 0
for _ in range(200):
    d = deque()
    for i in range(500):
        d.append(i)
        if i % 3 == 0:
            d.popleft()
    total += len(d)
    m = Counter(words)
    total += m["ab"]
    for pair in m.most_common(1):
        total += pair[1]
print(total)
