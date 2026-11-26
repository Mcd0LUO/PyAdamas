# 0.572：str.isascii()（方法表此前缺这一项）
s = "abc"
t = "abc\u00e9"
e = ""
print(s.isascii(), t.isascii(), e.isascii())

# --- expect ---
# True False True
