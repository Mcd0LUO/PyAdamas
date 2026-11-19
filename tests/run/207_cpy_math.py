# CPython 桥先验：from python import math —— float/int 返回、异常映射。
# 走 python3 worker 子进程 IPC（rt_cpy.c），不是原生 math 降级。
from python import math

print(math.sqrt(2.0))
print(math.sqrt(144.0))
print(math.floor(3.7))
print(math.factorial(6))

try:
    math.sqrt(-1.0)
    print("NO EXC")
except ValueError:
    print("caught ValueError")

try:
    math.sqrt(-2.0)
    print("NO EXC")
except Exception:
    print("caught as Exception")
# --- expect ---
# 1.4142135623730951
# 12.0
# 3
# 720
# caught ValueError
# caught as Exception
