# CPython 桥先验：str 返回用例 + as 别名 + unknown 结果链式传参
# （内层结果 unknown → 动态探针 marshalling）。
from python import os as cpyos
from python import json as cjson

print(cpyos.path.basename("/a/b/c.txt"))
print(cpyos.path.join("a", "b"))
print(len(cpyos.getcwd()) > 0)
print(cjson.dumps(cpyos.path.basename("/a/b/c.txt")))
print(cjson.dumps(cpyos.path.join("x", "y")))
# --- expect ---
# c.txt
# a/b
# True
# "c.txt"
# "x/y"
