"""queue 基准：Queue / LifoQueue / PriorityQueue 存取。"""
import queue

total = 0
r = 0
while r < 120:
    q = queue.Queue()
    i = 0
    while i < 300:
        q.put_nowait(i)
        i += 1
    i = 0
    while i < 300:
        total += q.get_nowait()
        i += 1
    lq = queue.LifoQueue()
    i = 0
    while i < 200:
        lq.put_nowait(i)
        i += 1
    i = 0
    while i < 200:
        total += lq.get_nowait()
        i += 1
    pq = queue.PriorityQueue()
    i = 0
    while i < 100:
        pq.put_nowait(100 - i)
        i += 1
    i = 0
    while i < 100:
        total += pq.get_nowait()
        i += 1
    r += 1
print(total)
