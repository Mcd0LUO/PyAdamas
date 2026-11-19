# cpy_ipc —— CPython 桥 IPC 延迟实测（先验原型，见 docs/cpybridge-design.md）
#
# 用法：pyadamas run --rebuild -O2 -o /tmp/cpy_ipc bench/cpy_ipc.py && /tmp/cpy_ipc
# 对比三类负载的单次调用成本：
#   轻：os.getpid（worker 侧 ~0.1µs 的函数）
#   轻：math.sqrt（浮点盒返回）
#   中：json.dumps（100 元素 list  marshalling + worker 序列化）
# 每类各跑 IPC 版与原生版；另测首次调用（含 worker 懒拉起）。
from python import os as cpyos
from python import math as cpymath
from python import json as cjson
import os
import math
import json
import time

N = 3000

# 首次调用：含 fork/exec + python3 解释器启动 + import。
t0 = time.perf_counter()
cpyos.getpid()
t1 = time.perf_counter()
print("first call (spawn) ms:", (t1 - t0) * 1e3)

t0 = time.perf_counter()
for i in range(N):
    cpyos.getpid()
t1 = time.perf_counter()
print("ipc getpid us:", (t1 - t0) * 1e6 / N)

t0 = time.perf_counter()
for i in range(N):
    os.getpid()
t1 = time.perf_counter()
print("native getpid us:", (t1 - t0) * 1e6 / N)

t0 = time.perf_counter()
for i in range(N):
    cpymath.sqrt(2.0)
t1 = time.perf_counter()
print("ipc sqrt us:", (t1 - t0) * 1e6 / N)

t0 = time.perf_counter()
for i in range(N):
    math.sqrt(2.0)
t1 = time.perf_counter()
print("native sqrt us:", (t1 - t0) * 1e6 / N)

xs = [i for i in range(100)]
t0 = time.perf_counter()
for i in range(N):
    cjson.dumps(xs)
t1 = time.perf_counter()
print("ipc json.dumps(100) us:", (t1 - t0) * 1e6 / N)

t0 = time.perf_counter()
for i in range(N):
    json.dumps(xs)
t1 = time.perf_counter()
print("native json.dumps(100) us:", (t1 - t0) * 1e6 / N)
