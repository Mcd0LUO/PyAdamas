# 441: big 容器全链路 —— in / index / count / remove / sort / sorted / 相等比较
# 以及 dict/set 的 big 键。
#
# 关键点：这些比较都发生在**运行时容器代码**里，编译器只传一个「怎么比」
# 的种类：查找用 key_kind（4 = big，按值比较），排序用描述符字符 'b'，
# dict/set 键用哈希种类 9（raw-or-big，按值哈希 + pa_big_eq）。


xs: list[big] = [1180591620717411303425, 5, 1180591620717411303424]
print(1180591620717411303424 in xs)
print(6 in xs)
print(xs.index(5))
print(xs.count(5))
print(xs.count(1180591620717411303424))

ys: list[big] = [1180591620717411303425, 5, 1180591620717411303424]
ys.remove(5)
print(ys)

print(sorted(xs))
zs: list[big] = [3, 1, 2]
zs.sort()
print(zs)

print([7] == [7])
a: list[big] = [1180591620717411303424]
b: list[big] = [1180591620717411303424]
print(a == b)
print(a == [5])

d: dict[big, str] = {}
d[1180591620717411303424] = "big"
k: big = 1180591620717411303424
print(d[k])
print(len(d))
print(k in d)
print(5 in d)

s: set[big] = {1180591620717411303424, 5}
print(len(s))
print(1180591620717411303424 in s)
print(6 in s)
# --- expect ---
# True
# False
# 1
# 1
# 1
# [1180591620717411303425, 1180591620717411303424]
# [5, 1180591620717411303424, 1180591620717411303425]
# [1, 2, 3]
# True
# True
# False
# big
# 1
# True
# False
# 2
# True
# False
