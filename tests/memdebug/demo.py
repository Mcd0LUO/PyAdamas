# demo.py —— 证明 PYADAMAS_CANARY / PYADAMAS_GUARD 真的能抓到堆越界写。
#
# 用法（从仓库根目录）：
#   PYADAMAS_CANARY=1 build/lead/pyadamas build --rebuild -o /tmp/demo tests/memdebug/demo.py
#   PYADAMAS_CANARY=1 /tmp/demo        # → 释放时 panic，指出 canary 被踩
#   PYADAMAS_GUARD=1 ...               # → 越界写那一刻 SIGSEGV
#   不带开关 ...                       # → 越界写静默通过（盲区）
#
# 布局：len=63 的堆字符串 payload = sizeof(PyAdamasStr)+len+1 = 104 字节，
# 字符区 data 在 payload+40，data[63] 是结尾 NUL，**data[64] 正好是对象
# 尾部之后第一个字节**（canary 保护带首字节 / guard 页之前的尾隙）。
import os
import sys

cimport memdbg as m

length = 63
s = "A" * length
print("len", len(s))

guard = os.getenv("PYADAMAS_GUARD")
if guard is not None and guard != "0":
    # guard 模式：写 40 字节，保证越过尾隙撞上 PROT_NONE 页。
    m.stomp(s, length + 1, 40)
else:
    # canary 模式：只踩保护带前 8 字节（多写可能踩到相邻 malloc 块）。
    m.stomp(s, length + 1, 8)
print("still alive after oob write")

# 退出时模块槽释放 → 触发 canary 校验（canary 模式下 panic）。
print("first chars", s[:3])
