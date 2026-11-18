# CPython 桥先验：异常映射 —— 按 MRO 取第一个 pyadamas 内置名。
# JSONDecodeError 不在内置名表 → 沿 MRO 落到 ValueError。
# 注意：worker 内部对象（迭代器等）不能跨边界，见 210 旧设计的教训。
from python import math
from python import json as cjson
from python import operator

try:
    math.sqrt(-1.0)
except ValueError:
    print("ValueError")

try:
    cjson.loads("{bad")
except ValueError:
    print("JSONDecodeError -> ValueError")

try:
    math.sqrt("x")
except TypeError:
    print("TypeError")

try:
    operator.truediv(1, 0)
except ZeroDivisionError:
    print("ZeroDivisionError")

print(cjson.loads("[1]"))
# --- expect ---
# ValueError
# JSONDecodeError -> ValueError
# TypeError
# ZeroDivisionError
# [1]
