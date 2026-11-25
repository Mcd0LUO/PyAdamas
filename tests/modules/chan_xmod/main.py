# 0.546：跨模块 channel（Chan[T] 标注）+ 跨模块 go。
import vtmod

ch = make_chan(int, 1)
h = go vtmod.producer(ch)
print(ch.recv())
print(vtmod.plain(7))

# --- expect ---
# 42
# 14
