"""queue —— FIFO/LIFO/优先级队列（纯 pyadamas 源码，stdlib3）。

覆盖 Queue / LifoQueue / PriorityQueue / SimpleQueue 与 Empty / Full。
接口同 CPython：put / put_nowait / get / get_nowait / qsize / empty / full /
task_done / join，maxsize=0 表示不限。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- **线程安全**：pa 运行时是 M=1 协作式（vthread 单 OS 线程、全局态无锁），
  所以这里不加锁；跨 vthread 访问是协作切换、不会真并发。阻塞口径用
  time.sleep 轮询（sleep 是 vthread 的让出点），功能上等价于 CPython 的
  Condition 等待，但**不是** OS 线程安全的实现。
- 阻塞 put/get 在单线程里会一直等（同 CPython 会死锁的用法）；测试只钉
  put_nowait/get_nowait 与带 timeout 的路径。
- PriorityQueue 用 heapq，元素按 `<` 比较（与 CPython 的堆一致）。
"""

import heapq
import time

_POLL = 0.001


class Empty(Exception):
    """队列为空（get_nowait / get(timeout=...)）。"""


class Full(Exception):
    """队列已满（put_nowait / put(timeout=...)，仅 maxsize>0）。"""


class Queue:
    """FIFO 队列。"""

    maxsize: int
    _items: list[unknown]
    _head: int
    _unfinished: int

    def __init__(self, maxsize: int = 0) -> None:
        self.maxsize = maxsize
        self._items = []
        self._head = 0
        self._unfinished = 0

    def qsize(self) -> int:
        return len(self._items) - self._head

    def empty(self) -> bool:
        return self.qsize() == 0

    def full(self) -> bool:
        if self.maxsize <= 0:
            return False
        return self.qsize() >= self.maxsize

    def _enqueue(self, item: unknown) -> None:
        self._items.append(item)

    def _dequeue(self) -> unknown:
        item = self._items[self._head]
        self._head = self._head + 1
        if self._head > 64 and self._head * 2 > len(self._items):
            keep: list[unknown] = []
            i = self._head
            while i < len(self._items):
                keep.append(self._items[i])
                i += 1
            self._items = keep
            self._head = 0
        return item

    def put(self, item: unknown, block: bool = True,
            timeout: float = -1.0) -> None:
        if not block:
            if self.full():
                raise Full
            self._enqueue(item)
            self._unfinished = self._unfinished + 1
            return
        if timeout < 0.0:
            while self.full():
                time.sleep(_POLL)
        else:
            end = time.monotonic() + timeout
            while self.full():
                if time.monotonic() >= end:
                    raise Full
                time.sleep(_POLL)
        self._enqueue(item)
        self._unfinished = self._unfinished + 1

    def put_nowait(self, item: unknown) -> None:
        self.put(item, False, -1.0)

    def get(self, block: bool = True, timeout: float = -1.0) -> unknown:
        if not block:
            if self.empty():
                raise Empty
            return self._dequeue()
        if timeout < 0.0:
            while self.empty():
                time.sleep(_POLL)
        else:
            end = time.monotonic() + timeout
            while self.empty():
                if time.monotonic() >= end:
                    raise Empty
                time.sleep(_POLL)
        return self._dequeue()

    def get_nowait(self) -> unknown:
        return self.get(False, -1.0)

    def task_done(self) -> None:
        if self._unfinished <= 0:
            raise ValueError("task_done() called too many times")
        self._unfinished = self._unfinished - 1

    def join(self) -> None:
        while self._unfinished > 0:
            time.sleep(_POLL)


class LifoQueue(Queue):
    """后进先出队列（栈）。"""

    def _enqueue(self, item: unknown) -> None:
        self._items.append(item)

    def _dequeue(self) -> unknown:
        item = self._items.pop()
        self._head = 0
        return item

    def qsize(self) -> int:
        return len(self._items)

    def empty(self) -> bool:
        return len(self._items) == 0


class PriorityQueue(Queue):
    """优先级队列（heapq 最小堆）。"""

    def _enqueue(self, item: unknown) -> None:
        heapq.heappush(self._items, item)

    def _dequeue(self) -> unknown:
        item = heapq.heappop(self._items)
        self._head = 0
        return item

    def qsize(self) -> int:
        return len(self._items)

    def empty(self) -> bool:
        return len(self._items) == 0


class SimpleQueue:
    """无上限、无 task_done 的简单 FIFO（同 CPython queue.SimpleQueue 面）。"""

    _items: list[unknown]
    _head: int

    def __init__(self) -> None:
        self._items = []
        self._head = 0

    def qsize(self) -> int:
        return len(self._items) - self._head

    def empty(self) -> bool:
        return self.qsize() == 0

    def put(self, item: unknown, block: bool = True, timeout: float = -1.0) -> None:
        self._items.append(item)

    def put_nowait(self, item: unknown) -> None:
        self._items.append(item)

    def get(self, block: bool = True, timeout: float = -1.0) -> unknown:
        while self.empty():
            time.sleep(_POLL)
        item = self._items[self._head]
        self._head = self._head + 1
        return item

    def get_nowait(self) -> unknown:
        if self.empty():
            raise Empty
        item = self._items[self._head]
        self._head = self._head + 1
        return item
