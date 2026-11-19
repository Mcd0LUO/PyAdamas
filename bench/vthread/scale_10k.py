# 10k 并发阻塞 vthread：内存账（RSS/VSZ）与唤醒收尾
import time

def waiter(ch):
    ch.recv()

def mem_kb():
    f = open("/proc/self/statm")
    s = f.read()
    f.close()
    parts = s.split()
    return int(parts[1]) * 4

def vsz_kb():
    f = open("/proc/self/statm")
    s = f.read()
    f.close()
    parts = s.split()
    return int(parts[0]) * 4

ch = make_chan(int, 0)
base_r = mem_kb()
base_v = vsz_kb()
N = 10000
i = 0
while i < N:
    go waiter(ch)
    i = i + 1
sleep(200)
after_r = mem_kb()
after_v = vsz_kb()
print("vthreads", N)
print("rss_delta_kb", after_r - base_r)
print("kb_per_vthread", (after_r - base_r) / N)
print("vsz_delta_mb", (after_v - base_v) / 1024)
i = 0
while i < N:
    ch.send(i)
    i = i + 1
sleep(200)
print("wake_done_rss_kb", mem_kb())
