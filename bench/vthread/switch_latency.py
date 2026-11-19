# 切换/无缓冲握手延迟：两个 vthread 经无缓冲 channel 乒乓 N 轮
import time

def ping(ch1, ch2, n):
    i = 0
    while i < n:
        ch1.recv()
        ch2.send(1)
        i = i + 1

def pong(ch1, ch2, n):
    i = 0
    while i < n:
        ch1.send(1)
        ch2.recv()
        i = i + 1

a = make_chan(int, 0)
b = make_chan(int, 0)
N = 100000
t0 = time.perf_counter()
hp = go ping(a, b, N)
hq = go pong(a, b, N)
join(hp)
join(hq)
t1 = time.perf_counter()
print("rounds", N)
print("ns_per_round", (t1 - t0) * 1e9 / N)
