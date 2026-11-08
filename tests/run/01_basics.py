# 标量运算、字符串、f-string

print("hello, py23")
print(1 + 2)
print(3.5 * 2)
print(7 // 3, 7 % 3, -7 // 3, -7 % 3)
print(7 / 2)
print(2 ** 10)
print(1 < 2)
print(None)
name = "py23"
print(f"{name} has {len(name)} chars")

# --- expect ---
# hello, py23
# 3
# 7.0
# 2 1 -3 2
# 3.5
# 1024
# True
# None
# py23 has 4 chars
