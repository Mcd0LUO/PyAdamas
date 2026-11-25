from collections import deque

d: deque = deque()
d.append(1)
d.append(2)
d.appendleft(0)
print(len(d), d[0], d[2])

# --- expect ---
# 3 0 2
