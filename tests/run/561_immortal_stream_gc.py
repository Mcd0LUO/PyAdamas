# 回归：sys.stdout / sys.stderr 是 IMMORTAL（rc = INT64_MIN）对象，但它们只挂在
# C 静态数组 sys_stream_files 上，既不在 gc_globals 里也不在栈/寄存器上 —— GC 的
# 根扫描看不到。曾经 minor GC 的 trial-deletion 按「rc > 0 才算活」判定，把它们
# 当垃圾 free 掉，之后第一次 write 就崩（object has no attribute 'write'）。
#
# 这里先触达标准流，让文件对象进入新生代；再制造足够多的堆对象触发 minor GC；
# 最后继续写 stdout。修复前会崩（或被保守根侥幸兜住），修复后稳定通过。
import sys

sys.stdout.flush()
sys.stderr.flush()

class Sym:
    kind: int
    name: str
    line: int

i: int = 0
while i < 120000:
    s: Sym = Sym()
    s.kind = 12
    s.name = "func_" + str(i)
    s.line = i
    i = i + 1

sys.stdout.write("alive\n")
print("ok")

# --- expect ---
# alive
# ok
