# CPython 桥先验：json dumps/loads —— str 返回、容器 marshalling、
# 结果解码成真 list/dict（float 盒 + repr 打印）。
from python import json as cjson

print(cjson.dumps([1, 2, 3]))
print(cjson.dumps({"a": 1, "b": [2, 3]}))
print(cjson.dumps((1, "x", 2.0)))
print(cjson.dumps("héllo"))
xs = cjson.loads("[1, 2.5, \"x\"]")
print(xs)
print(len(xs))
d = cjson.loads("{\"k\": 7, \"j\": [2, 3]}")
print(d)
print(len(d))
# --- expect ---
# [1, 2, 3]
# {"a": 1, "b": [2, 3]}
# [1, "x", 2.0]
# "h\u00e9llo"
# [1, 2.5, 'x']
# 3
# {'k': 7, 'j': [2, 3]}
# 2
