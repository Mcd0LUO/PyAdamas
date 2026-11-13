"""heapq 模块的 pyadamas 子集（纯 pyadamas 源码）。

支持 heappush / heappop / heapify / heappushpop / heapreplace /
nlargest / nsmallest。元素按 `<` 比较；空堆 heappop 抛 IndexError。
列表元素静态类型保持 unknown，调用方按需收窄。
"""


def _siftdown(heap: list, start: int, pos: int) -> None:
    newitem = heap[pos]
    while pos > start:
        parent = (pos - 1) >> 1
        if newitem < heap[parent]:
            heap[pos] = heap[parent]
            pos = parent
        else:
            break
    heap[pos] = newitem


def _siftup(heap: list, pos: int) -> None:
    n = len(heap)
    newitem = heap[pos]
    child = 2 * pos + 1
    while child < n:
        right = child + 1
        if right < n and heap[right] < heap[child]:
            child = right
        if heap[child] < newitem:
            heap[pos] = heap[child]
            pos = child
            child = 2 * pos + 1
        else:
            break
    heap[pos] = newitem


def heappush(heap: list, item) -> None:
    heap.append(item)
    _siftdown(heap, 0, len(heap) - 1)


def heappop(heap: list):
    n = len(heap)
    if n == 0:
        raise IndexError("index out of range")
    last = heap.pop()
    if n == 1:
        return last
    top = heap[0]
    heap[0] = last
    _siftup(heap, 0)
    return top


def heapify(heap: list) -> None:
    n = len(heap)
    i = (n - 1) // 2
    while i >= 0:
        _siftup(heap, i)
        i -= 1


def heappushpop(heap: list, item):
    if len(heap) == 0:
        return item
    if heap[0] < item:
        top = heap[0]
        heap[0] = item
        _siftup(heap, 0)
        return top
    return item


def heapreplace(heap: list, item):
    if len(heap) == 0:
        raise IndexError("index out of range")
    top = heap[0]
    heap[0] = item
    _siftup(heap, 0)
    return top


def nlargest(n: int, iterable: list) -> list:
    if n <= 0:
        return []
    xs = sorted(iterable)
    if n >= len(xs):
        xs.reverse()
        return xs
    out = xs[len(xs) - n:]
    out.reverse()
    return out


def nsmallest(n: int, iterable: list) -> list:
    if n <= 0:
        return []
    xs = sorted(iterable)
    if n >= len(xs):
        return xs
    return xs[:n]
