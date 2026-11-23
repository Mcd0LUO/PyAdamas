# io.StringIO / BytesIO 常用 API 对拍。
import io
s = io.StringIO()
s.write("hello")
print("val", s.getvalue())
s.seek(0)
print("read", s.read())
b = io.BytesIO()
b.write(b"abc")
print("bval", b.getvalue())
b.seek(0)
print("bread", b.read())

# --- expect ---
# val hello
# read hello
# bval b'abc'
# bread b'abc'
