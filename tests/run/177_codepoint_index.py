# 码点下标/切片/迭代语义（S2）
s = "中文"
print(s[0])
print(s[1])
print(s[-1])
print(s[-2])

# 混合 ASCII / 非 ASCII
m = "a中b"
print(m[0], m[1], m[2])
print(m[-1], m[-2], m[-3])

# 切片
print(s[0:1])
print(s[0:2])
print(s[:])
print(s[::-1])
print(m[1:])
print(m[:2])
print(m[::2])
print(m[2:0:-1])

# 迭代
n = 0
acc = ""
for ch in s:
    n += 1
    acc = acc + ch
print(n, acc)

# enumerate / zip
for i, ch in enumerate(s):
    print(i, ch)
for x, y in zip("ab", "中文"):
    print(x, y)

# 推导式
print([ch for ch in s])
print(len([ch for ch in "中文abc"]))

# list() / tuple() / sorted / reversed
print(list(s))
print(tuple(s))
print(sorted("cba"))
print(list(reversed(s)))

# bytes 对照：全部字节语义
bs = b"\xe4\xb8\xad\xe6\x96\x87"
print(len(bs))
print(bs[0])
print(bs[-1])
print(bs[0:2])
print(bs[::-1])
print(list(bs))
print(bs + b"x")

# bytes 下标/切片仍是 int/bytes
print(type(bs[0]).__name__)
print(len(bs[0:2]))

# 空串边界
print(len(""), len(""[0:0]))

# --- expect ---
# 中
# 文
# 文
# 中
# a 中 b
# b 中 a
# 中
# 中文
# 中文
# 文中
# 中b
# a中
# ab
# b中
# 2 中文
# 0 中
# 1 文
# a 中
# b 文
# ['中', '文']
# 5
# ['中', '文']
# ('中', '文')
# ['a', 'b', 'c']
# ['文', '中']
# 6
# 228
# 135
# b'\xe4\xb8'
# b'\x87\x96\xe6\xad\xb8\xe4'
# [228, 184, 173, 230, 150, 135]
# b'\xe4\xb8\xad\xe6\x96\x87x'
# int
# 2
# 0 0
