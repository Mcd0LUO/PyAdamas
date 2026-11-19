# spawn+join 延迟：空函数 N 次（栈池复用路径）
import time

def noop():
    pass

N = 20000
t0 = time.perf_counter()
i = 0
while i < N:
    h = go noop()
    join(h)
    i = i + 1
t1 = time.perf_counter()
print("spawn_join_n", N)
print("ns_per_spawn_join", (t1 - t0) * 1e9 / N)
