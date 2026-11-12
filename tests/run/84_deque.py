# collections.deque：两端进出、rotate、maxlen、迭代与 list() 转换
import collections

d = collections.deque()
d.append(1)
d.append(2)
d.appendleft(0)
print(len(d))
print(d[0], d[1], d[2])
print(d.pop())
print(d.popleft())
print(len(d))

e = collections.deque([1, 2, 3])
print(len(e))
e.rotate(1)
print(e[0], e[1], e[2])
e.rotate(-1)
print(e[0], e[1], e[2])

m = collections.deque([], 2)
m.append(1)
m.append(2)
m.append(3)
print(len(m), m[0], m[1])

total = 0
for x in e:
    total += x
print(total)

print(list(e))
# --- expect ---
# 3
# 0 1 2
# 2
# 0
# 1
# 3
# 3 1 2
# 1 2 3
# 2 2 3
# 6
# [1, 2, 3]
