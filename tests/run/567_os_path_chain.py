# os.path 属性链（0.484，pya-lsp 缺口 2 的配套）：子模块属性链端到端可用。
# 之前 `os.path.join(...)` 在 check/build 都报 "module 'os' has no attribute 'path'"，
# 只能写 `from os.path import join`。现在按完整模块名（os.path）解析成员，
# 与 from-import 同口径降级。同时钉住子模块缺成员仍要报错。
import os

print(os.path.join("a", "b"))
print(os.path.basename("/tmp/x/y.txt"))
print(os.path.exists("/tmp"), os.path.isdir("/tmp"))
print(os.path.split("/tmp/x/y.txt"))
# --- expect ---
# a/b
# y.txt
# True True
# ('/tmp/x', 'y.txt')
