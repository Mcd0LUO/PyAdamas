# 0.577：bytes.isascii() / bytes.rindex()（runtime 早就实现，类型检查没放行）
print(b"ab".isascii())
print(b"\xff".isascii())
print(b"abcabc".rindex(b"b"))
print(b"abc".rindex(b"b", 0, 2))

try:
    b"abc".rindex(b"z")
except ValueError as e:
    print("ValueError:", e)

# --- expect ---
# True
# False
# 4
# 1
# ValueError: subsection not found
