# 码点长度语义（S1）：len 按码点、len_bytes 按字节、bytes 完全不变
s = "中文"
print(len(s))
print(len("abc"))
print(len(""))
print(len("中"))
print(len("a中b"))
print(len("😀"))
print(len("中文abc"))

# 字节长度出口
print(len_bytes(s))
print(len_bytes("abc"))
print(len_bytes(""))
print(len_bytes("😀"))

# bytes 语义完全不变：len 就是字节数
print(len(b"中文"))
print(len(b"abc"))
print(len_bytes(b"abc"))

# 字面量 vs 运行时拼接
lit = "中文"
cat = "中" + "文"
print(len(lit), len(cat), lit == cat)

# 从 ASCII 片段拼出非 ASCII
parts = "a" + "中" + "b"
print(len(parts), len_bytes(parts))

# 长串（超过 SSO 阈值 7 字节）
long = "中文" * 100
print(len(long), len_bytes(long))
print(len(long) * 3 == len_bytes(long))

# 空串/单字符边界
print(len(" "), len_bytes(" "))
print(len("\n"), len_bytes("\n"))

# 4 字节 emoji 混合
mix = "a😀b中c"
print(len(mix), len_bytes(mix))
print(mix[0], mix[1], mix[2], mix[3], mix[4])

# --- expect ---
# 2
# 3
# 0
# 1
# 3
# 1
# 5
# 6
# 3
# 0
# 4
# 6
# 3
# 3
# 2 2 True
# 3 5
# 200 600
# True
# 1 1
# 1 1
# 5 10
# a 😀 b 中 c
