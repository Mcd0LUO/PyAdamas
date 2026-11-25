# 跨模块 isinstance（0.531）：类 id 是限定名的 FNV-1a 64，定义模块与导入方
# 各算一次、天然一致 —— import 进来的类也能走整数 MRO 比较。
from value import Value

x: Value = Value(2.0)
print(isinstance(x, Value))
print(x.get())

# --- expect ---
# True
# 2.0
