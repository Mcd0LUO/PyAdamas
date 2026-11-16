# 字符串方法在码点语义下的关键行为（S3 未做的方法只测不破坏的部分）
s = "中文abc"
print(len(s), len_bytes(s))

# find/rfind 返回码点下标
print(s.find("abc"))
print(s.find("文"))
print(s.rfind("a"))
print(s.find("zz"))
print(s.find("文", 1))
print(s.find("a", 3))

# count / startswith / endswith / in
print(s.count("中"), s.count("zz"), s.count(""))
print(s.startswith("中"), s.endswith("bc"))
print("文" in s, "x" in s)

# 切片 + 方法组合
print(s[2:], s[:2], s[1:3])

# strip（chars 按码点集合匹配）
print("中中文中".strip("中"))
print("  中文  ".strip())

# split / join
print("中,文,a".split(","))
print(",".join(["中", "文"]))
print(len(",".join(["中", "文"])))

# replace（空 old 必须按码点插入，不能切开多字节序列）
print("中文".replace("", "-"))
print(len("中文".replace("", "-")))
print("中文".replace("中", "X"))

# upper/lower 只动 ASCII，pub 不变
print("中a文".upper(), len("中a文".upper()))
print("中A文".lower(), len("中A文".lower()))

# capitalize / title 不破坏多字节序列
print("中a文".capitalize())
print("中a文 b".title())  # 非 ASCII 按非字母：已知差异

# 宽度方法按码点
print("中".ljust(3, "-"))
print("中".rjust(3, "-"))
print("中".center(5, "-"))
print("中".zfill(3))
print(len("中".ljust(3, "-")))

# removeprefix / removesuffix
print("中文abc".removeprefix("中"))
print("中文abc".removesuffix("bc"))
print(len("中文abc".removeprefix("中")))

# 比较/哈希/字典 key
print("中" < "文", "a" < "中")
d = {"中": 1, "文": 2}
print(d["中"], d["文"])
print(hash("中") == hash("中"))

# ord/chr 按码点
print(ord("中"), chr(20013), len(chr(20013)))
print(ord("😀"), chr(128512))

# repr 与 % 格式化
print(repr("中"))
print("%s" % "中")
print("%s-%s" % ("中", "文"))

# --- expect ---
# 5 9
# 2
# 1
# 2
# -1
# 1
# -1
# 1 0 6
# True True
# True False
# abc 中文 文a
# 文
# 中文
# ['中', '文', 'a']
# 中,文
# 3
# -中-文-
# 5
# X文
# 中A文 3
# 中a文 3
# 中a文
# 中a文 B
# 中--
# --中
# --中--
# 00中
# 3
# 文abc
# 中文a
# 4
# True True
# 1 2
# True
# 20013 中 1
# 128512 😀
# '中'
# 中
# 中-文
