from collections import deque


def make(a: int, b: int) -> deque:
    d: deque = deque()
    d.append(a)
    d.append(b)
    return d
