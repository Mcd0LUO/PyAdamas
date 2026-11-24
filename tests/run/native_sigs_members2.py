# sys.getrecursionlimit / setrecursionlimit（0.511）：pyadamas 没有运行期递归
# 限制，这两个函数只保存/查询一个值（默认 1000，与 CPython 一致），输出与
# CPython 3.14 逐字节相同。
import sys

print(sys.getrecursionlimit())
sys.setrecursionlimit(5000)
print(sys.getrecursionlimit())
sys.setrecursionlimit(1000)
print(sys.getrecursionlimit())

# --- expect ---
# 1000
# 5000
# 1000
