# for 循环长度缓存回归：体内增删元素时按 CPython 的「每步重读 len」语义
a = [1, 2]
for x in a:
    a.pop()
print("pop-all", a)

b = [1, 2, 3]
seen: list[int] = []
for x in b:
    seen.append(x)
    if len(b) < 6:
        b.append(x + 10)
print("append", seen, b)

c = [10, 20, 30, 40]
for x in c:
    if x == 20:
        del c[2]
print("del", c)

d = [1, 2, 3, 4]
for x in d:
    if x == 1:
        d[1:3] = []
print("slice", d)

# 无副作用的循环仍走缓存长度（IndexLoadUnchecked 路径不受影响）
e = [5, 6, 7]
tot = 0
for x in e:
    tot += x
print("clean", tot)

# 元组不可变：长度补丁不参与，行为照旧
t = (1, 2, 3)
s = 0
for x in t:
    s += x
print("tuple", s)

# 字符串不可变
n = 0
for ch in "abc":
    n += 1
print("str", n)

# 函数体内（非入口）同样成立
def shrink(xs: list[int]) -> int:
    k = 0
    for v in xs:
        xs.pop()
        k += 1
    return k

print("func", shrink([1, 2, 3, 4]))

# --- expect ---
# pop-all [1]
# append [1, 2, 3, 11, 12, 13] [1, 2, 3, 11, 12, 13]
# del [10, 20, 40]
# slice [1, 4]
# clean 18
# tuple 6
# str 3
# func 2
