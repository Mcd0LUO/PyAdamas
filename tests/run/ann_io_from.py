# 0.551：项目代码里 `from io import StringIO` 的类型解析（check 侧也要认）。
from io import StringIO

s: StringIO = StringIO()
s.write("ab")
print(s.getvalue())

# --- expect ---
# ab
