from collections import deque, Counter

d = deque([1, 2, 3])
d.append(4)
d.appendleft(0)
print(len(d), d[0], d[-1])
print(d.count(2), d.index(3))
d.remove(2)
print(list(d))
d.insert(1, 9)
print(list(d))
d.reverse()
print(list(d))
e = d.copy()
e.append(99)
print(list(d), list(e))

m = deque([1, 2, 3], maxlen=3)
m.append(4)
print(list(m), m.maxlen)
m.appendleft(0)
print(list(m))
try:
    m.insert(0, 5)
except IndexError:
    print("IndexError")

words = ["a", "b", "a"]
print(Counter(words)["a"], Counter(words).most_common(1))

# --- expect ---
# 5 0 4
# 1 3
# [0, 1, 3, 4]
# [0, 9, 1, 3, 4]
# [4, 3, 1, 9, 0]
# [4, 3, 1, 9, 0] [4, 3, 1, 9, 0, 99]
# [2, 3, 4] 3
# [0, 2, 3]
# IndexError
# 2 [('a', 2)]
