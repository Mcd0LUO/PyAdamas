# 523_native_sigs.py —— 声明式原生签名（stdlib/sigs/<module>.pysig）端到端
#
# 覆盖 P0「签名从 C++ 硬编码挪进数据」的端到端行为：模块 time 的签名现在由
# stdlib/sigs/time.pysig 提供（typecheck/builtin_sigs.cpp 里的 time 分支已删除），
# 包括普通函数、带默认值的可选参数、tuple[int, ...] 返回类型、可变参，以及
# from-import 的原生名解析（lower/import.cpp 的 native_fn 走声明表）。

import time
from time import monotonic, perf_counter, sleep

print("time_positive", time.time() > 0)
print("monotonic_positive", time.monotonic() > 0)
print("perf_positive", time.perf_counter() > 0)

# 声明的形参类型（secs: float）要真的 coerce：传 int 0 也必须能睡。
t0 = time.monotonic()
sleep(0)
sleep(0.01)
print("sleep_waited", time.monotonic() - t0 >= 0.005)

# from-import 出来的是同一个原生入口（native_fn 反查）
x = time.perf_counter()
print("from_import_positive", x > 0 and monotonic() > 0 and perf_counter() > 0)

# 声明里的默认值（secs: float = -1e300 = 取当前时间）：无参调用要能走通。
now_l = time.localtime()
now_g = time.gmtime()
print("localtime_len", len(now_l) == 9)
print("gmtime_len", len(now_g) == 9)
# 声明里的 tuple[int, ...] 返回类型
print("gmtime_epoch", time.gmtime(0)[:6] == (1970, 1, 1, 0, 0, 0))

# 声明式常量（.pysig 的 const → builtin_value 的通用 hook，零 C++）
print("clock_realtime", time.CLOCK_REALTIME)
print("clock_monotonic", time.CLOCK_MONOTONIC)

# 可变参声明（*args: unknown）+ 手工装配的降级路径
print("strftime", time.strftime("%Y-%m-%d", (2000, 1, 2, 3, 4, 5, 0, 0, 0)))
print("mktime_positive", time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0)) > 0)

# --- expect ---
# time_positive True
# monotonic_positive True
# perf_positive True
# sleep_waited True
# from_import_positive True
# localtime_len True
# gmtime_len True
# gmtime_epoch True
# clock_realtime 0
# clock_monotonic 1
# strftime 2000-01-02
# mktime_positive True
