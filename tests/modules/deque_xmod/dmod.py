from collections import deque

d: deque[int] = deque()
d.append(1)
d.append(2)

def total(x: deque[int]) -> int:
    s: int = 0
    for v in x:
        s = s + v
    return s
