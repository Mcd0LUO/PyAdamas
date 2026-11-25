from collections import deque

d: deque[int] = deque()
d.append(5)
d.append(6)
print(len(d), d[1])

# --- expect ---
# 2 6
