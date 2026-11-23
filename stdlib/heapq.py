"""heapq 模块的 pyadamas 子集（纯 pyadamas 源码）。

支持 heappush / heappop / heapify / heappushpop / heapreplace /
nlargest / nsmallest（含 key=）/ merge。元素按 `<` 比较；
空堆 heappop / heapreplace 抛 IndexError。

语义边界（轻量标尺，见 docs/stdlib-gaps.md 第 8 节）：
- nlargest / nsmallest 的 key= 收函数值（object 形参）；同一个程序里
  先传 int 列表再传 float 列表会被 fresh_hole 定型成首个类型（PA 类型
  系统限制），混合数值类型调用需分属不同程序。
- merge(*iterables) 收可变数量的 list（CPython 收任意 iterable），返回
  list（CPython 返回生成器）；输入已排序，输出稳定合并。
- 列表元素静态类型保持 list（fresh_hole）；单独传 float 列表可用，
  但与 int 列表混用受上述 hole 限制。
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


def _keyed(values: list, key: object) -> list:
    """返回 [(key(x), x)] 这样的对列表（用两个平行 list 避免 tuple 排序）。"""
    ks: list = []
    xs: list = []
    if key is None:
        return values
    for x in values:
        ks.append(key(x))
        xs.append(x)
    return ks


def nlargest(n: int, iterable: list, key: object = None) -> list:
    if n <= 0:
        return []
    if key is None:
        xs = sorted(iterable)
        if n >= len(xs):
            xs.reverse()
            return xs
        out = xs[len(xs) - n:]
        out.reverse()
        return out
    # key= 口径：按 key 值排序，返回原元素。用平行列表保存 (k, x) 映射。
    ks: list = []
    for x in iterable:
        ks.append(key(x))
    order: list[int] = []
    i = 0
    while i < len(ks):
        order.append(i)
        i += 1
    # 按 key 值冒泡排序（稳定，元素少，nlargest 用于 Top-N）
    j = 0
    while j < len(order):
        k = j + 1
        while k < len(order):
            if ks[order[j]] < ks[order[k]]:
                t = order[j]
                order[j] = order[k]
                order[k] = t
            k += 1
        j += 1
    cnt = n
    if cnt > len(order):
        cnt = len(order)
    out: list = []
    i = 0
    while i < cnt:
        out.append(iterable[order[i]])
        i += 1
    return out


def nsmallest(n: int, iterable: list, key: object = None) -> list:
    if n <= 0:
        return []
    if key is None:
        xs = sorted(iterable)
        if n >= len(xs):
            return xs
        return xs[:n]
    ks: list = []
    for x in iterable:
        ks.append(key(x))
    order: list[int] = []
    i = 0
    while i < len(ks):
        order.append(i)
        i += 1
    j = 0
    while j < len(order):
        k = j + 1
        while k < len(order):
            if ks[order[k]] < ks[order[j]]:
                t = order[j]
                order[j] = order[k]
                order[k] = t
            k += 1
        j += 1
    cnt = n
    if cnt > len(order):
        cnt = len(order)
    out: list = []
    i = 0
    while i < cnt:
        out.append(iterable[order[i]])
        i += 1
    return out


def merge(*iterables: list) -> list:
    """合并多个已排序列表，返回排序后的 list（CPython 返回生成器）。"""
    out: list = []
    n = len(iterables)
    # 多列表归并：反复取各列表当前最小值，含稳定顺序（小索引优先）
    idx: list[int] = []
    i = 0
    while i < n:
        idx.append(0)
        i += 1
    remaining = n
    while remaining > 0:
        best = -1
        i = 0
        while i < n:
            if idx[i] < len(iterables[i]):
                if best < 0 or iterables[i][idx[i]] < iterables[best][idx[best]]:
                    best = i
            i += 1
        if best < 0:
            break
        out.append(iterables[best][idx[best]])
        idx[best] = idx[best] + 1
        if idx[best] >= len(iterables[best]):
            remaining = remaining - 1
    return out
