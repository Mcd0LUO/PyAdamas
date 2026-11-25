# 0.553：Deque 跨模块（tag 30 修正后恢复）。
from collections import deque
import dmod

print(len(dmod.d), dmod.d[0], dmod.d[1])
print(dmod.total(dmod.d))
dmod.d.append(3)
print(len(dmod.d), dmod.total(dmod.d))

e: deque[int] = deque()
e.append(10)
print(dmod.total(e))

# --- expect ---
# 2 1 2
# 3
# 3 6
# 10
