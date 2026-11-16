# 字面量 intern 按 (kind, 内容) 去重 + 字面量 vs 运行时拼接 + 混合 ASCII
# "中" 与 b"\xe4\xb8\xad" 字节相同但 pub 不同，必须各自持有正确的长度。

# 1) 同一内容同时以 str / bytes 出现
s1 = "中"
b1 = b"\xe4\xb8\xad"
print(len(s1), len_bytes(s1))
print(len(b1), len_bytes(b1))
print(len("中"), len(b"\xe4\xb8\xad"))

# 2) 同一 str 内容重复出现（应复用同一个静态对象）
a = "重复的字面量"
c = "重复的字面量"
print(a == c, len(a), len_bytes(a))

# 3) 字面量 vs 运行时拼接：长度必须一致
lit = "中文测试"
cat = "中" + "文" + "测试"
print(len(lit), len(cat), lit == cat, len_bytes(lit), len_bytes(cat))

# 4) 运行时构造的串（非字面量）也要有正确 pub
buf = ""
for ch in "abc":
    buf = buf + ch
print(len(buf), len_bytes(buf))
cn = ""
for ch in "中文":
    cn = cn + ch
print(len(cn), len_bytes(cn), cn)

# 5) 混合 ASCII/非 ASCII 的各种构造
x = "a" + "中" + "b" + "文" + "c"
print(len(x), len_bytes(x))
print(x[0], x[1], x[2], x[3], x[4])

# 6) 4 字节 emoji（字面量里直接写 UTF-8）
e = "😀"
print(len(e), len_bytes(e), ord(e))
e2 = "a" + e + "b"
print(len(e2), len_bytes(e2))
print(e2[0], e2[1], e2[2])
print(e * 2, len(e * 2), len_bytes(e * 2))

# 7) str/bytes 转换：encode/decode 后长度口径切换
print(len("中文".encode()), len_bytes("中文".encode()))
print(len(b"\xe4\xb8\xad".decode()), len_bytes(b"\xe4\xb8\xad".decode()))
print(len("abc".encode()), len("abc".encode()))

# 8) 从容器里取出的串同样正确
lst = ["中", "ab", "😀"]
for item in lst:
    print(len(item), len_bytes(item))

# --- expect ---
# 1 3
# 3 3
# 1 3
# True 6 18
# 4 4 True 12 12
# 3 3
# 2 6 中文
# 5 9
# a 中 b 文 c
# 1 4 128512
# 3 6
# a 😀 b
# 😀😀 2 8
# 6 6
# 1 3
# 3 3
# 1 3
# 2 2
# 1 4
