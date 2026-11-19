# CPython 桥先验：marshalling 子集（None/bool/int/float/str/bytes/
# list/tuple/dict）+ worker 复用（50 次循环调用同一 worker）。
from python import json as cjson
from python import base64
from python import operator

print(cjson.dumps(None))
print(cjson.dumps(True))
print(cjson.dumps(False))
print(base64.b64encode(b"hello"))
print(base64.b64decode("aGVsbG8="))
print(operator.add(2, 3))
print(operator.add(1.5, 2.25))
print(operator.concat([1, 2], [3]))
total = 0
for i in range(50):
    total = total + operator.add(i, 1)
print(total)
# --- expect ---
# null
# true
# false
# aGVsbG8=
# hello
# 5
# 3.75
# [1, 2, 3]
# 1275
