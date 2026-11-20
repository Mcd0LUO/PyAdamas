# queue 子集：FIFO/LIFO/优先级/SimpleQueue 与 Empty/Full
import queue

q = queue.Queue()
print(q.empty(), q.qsize(), q.full())
q.put(1)
q.put(2)
q.put_nowait(3)
print(q.qsize(), q.empty(), q.full())
print(q.get(), q.get_nowait(), q.get())
print(q.empty())
try:
    q.get_nowait()
except queue.Empty:
    print("empty")

q2 = queue.Queue(2)
q2.put("a")
q2.put("b")
print(q2.full(), q2.qsize())
try:
    q2.put_nowait("c")
except queue.Full:
    print("full")
try:
    q2.put("c", False)
except queue.Full:
    print("full2")
try:
    q2.put("c", True, 0.05)
except queue.Full:
    print("full3")
try:
    queue.Queue().get(True, 0.05)
except queue.Empty:
    print("empty timeout")
print(q2.get(), q2.get())

lq = queue.LifoQueue()
lq.put(1)
lq.put(2)
lq.put(3)
print(lq.get(), lq.get(), lq.get())

pq = queue.PriorityQueue()
pq.put(3)
pq.put(1)
pq.put(2)
print(pq.get(), pq.get(), pq.get())
pq.put_nowait(5)
print(pq.qsize(), pq.get_nowait())

sq = queue.SimpleQueue()
sq.put("x")
sq.put_nowait("y")
print(sq.qsize(), sq.get(), sq.get(), sq.empty())
try:
    sq.get_nowait()
except queue.Empty:
    print("simple empty")

tasks = queue.Queue()
tasks.put(1)
tasks.put(2)
print(tasks.qsize())
tasks.task_done()
tasks.task_done()
print(tasks.empty())
try:
    tasks.task_done()
except ValueError:
    print("task_done ValueError")
tasks.join()
print("join done")
print(issubclass(queue.LifoQueue, queue.Queue))


# --- expect ---
# True 0 False
# 3 False False
# 1 2 3
# True
# empty
# True 2
# full
# full2
# full3
# empty timeout
# a b
# 3 2 1
# 1 2 3
# 1 5
# 2 x y True
# simple empty
# 2
# False
# task_done ValueError
# join done
# True
