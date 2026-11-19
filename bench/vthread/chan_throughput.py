# 缓冲 channel 吞吐：cap 64，生产者/消费者各 N 个 int
import time

def producer(ch, n):
    i = 0
    while i < n:
        ch.send(i)
        i = i + 1

def consumer(ch, n):
    total = 0
    i = 0
    while i < n:
        total = total + ch.recv()
        i = i + 1
    return total

ch = make_chan(int, 64)
N = 300000
t0 = time.perf_counter()
hp = go producer(ch, N)
hq = go consumer(ch, N)
join(hp)
join(hq)
t1 = time.perf_counter()
print("items", N)
print("ns_per_item", (t1 - t0) * 1e9 / N)
