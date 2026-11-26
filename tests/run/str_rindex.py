# 0.573：str.rindex()（方法表 46/47 → 47/47）
print("abcabc".rindex("b"))
print("abc".rindex("b", 0, 2))
print("aaa".rindex("a"))

try:
    "abc".rindex("z")
except ValueError as e:
    print("ValueError:", e)

# --- expect ---
# 4
# 1
# 2
# ValueError: substring not found
