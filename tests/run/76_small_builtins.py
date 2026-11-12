# 小内置：slice() 作下标、vars() 取实例字段、dir() 列成员
xs = [1, 2, 3, 4, 5]
print(xs[slice(1, 3)])
print(xs[slice(3)])
print(xs[slice(1, 5, 2)])
print(xs[slice(-2, None)])
print(xs[slice(None, 2)])

s = "abcdef"
print(s[slice(1, 4)])
print(s[slice(None, None, 2)])

xs[slice(0, 2)] = [9, 8]
print(xs)
del xs[slice(0, 1)]
print(xs)


class Point:
    x: int = 1
    y: int = 2

    def norm(self) -> int:
        return self.x + self.y


p = Point()
p.x = 10
d = vars(p)
print(len(d), d["x"], d["y"])
print(sorted(d.keys()))
print(dir(p))


def local_names():
    a = 1
    b = 2
    return dir()


print(local_names())
print(dir()[0:0])
# --- expect ---
# [2, 3]
# [1, 2, 3]
# [2, 4]
# [4, 5]
# [1, 2]
# bcd
# ace
# [9, 8, 3, 4, 5]
# [8, 3, 4, 5]
# 2 10 2
# ['x', 'y']
# ['norm', 'x', 'y']
# ['a', 'b']
# []
