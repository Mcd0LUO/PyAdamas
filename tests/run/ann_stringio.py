# 0.550：io.StringIO / io.BytesIO 的 dotted 注解（内置模块里用 pya 源码实现的类）。
import io

s: io.StringIO = io.StringIO()
s.write("ab")
print(s.getvalue())
print(s.tell())
s.seek(0)
print(s.read(1))
s.close()

b: io.BytesIO = io.BytesIO()
b.write(b"xy")
print(b.getvalue())
print(b.tell())

t: io.StringIO = io.StringIO("hi")
print(t.readline())

# --- expect ---
# ab
# 2
# a
# b'xy'
# 2
# hi
