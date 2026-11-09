# 切片赋值：替换/插入/删除、负索引、扩展切片、字符串右值
l: list[int] = [0, 1, 2, 3, 4, 5]
l[1:3] = [10, 20, 30]
print(len(l))
for x in l:
    print(x)

l[2:5] = []
for x in l:
    print(x)

l[1:1] = [7, 8]
l[-1:] = [99]
for x in l:
    print(x)

l[::2] = [100, 200, 300]
for x in l:
    print(x)

s: list[str] = ["a", "b", "c"]
s[1:] = "XY"
print(len(s))
for c in s:
    print(c)

names: list[str] = ["a", "b", "c", "d"]
names[1:1] = ["x", "y"]
for c in names:
    print(c)
names[2:5] = []
for c in names:
    print(c)
names[0:1] = names
print(len(names))
for c in names:
    print(c)

neg: list[str] = ["a", "b", "c", "d"]
neg[::-1] = ["w", "x", "y", "z"]
for c in neg:
    print(c)
# --- expect ---
# 7
# 0
# 10
# 20
# 30
# 3
# 4
# 5
# 0
# 10
# 4
# 5
# 0
# 7
# 8
# 10
# 4
# 99
# 100
# 7
# 200
# 10
# 300
# 99
# 3
# a
# X
# Y
# a
# x
# y
# b
# c
# d
# a
# x
# d
# 5
# a
# x
# d
# x
# d
# z
# y
# x
# w
