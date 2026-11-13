# base64 标准库：编码/解码与 URL-safe 变体。
import base64

e = base64.b64encode(b"hello")
print(e.decode())
print(base64.b64decode(e).decode())

u = base64.urlsafe_b64encode(bytes([251, 255]))
print(u.decode())
print(base64.b64decode(u) == bytes([251, 255]))

empty = base64.b64encode(b"")
print(len(empty))
print(len(base64.b64decode(empty)))

# --- expect ---
# aGVsbG8=
# hello
# -_8=
# True
# 0
# 0
