# 366_fixed_int_container_coerce —— P1 收尾：定宽容器元素的字面量 coerce。
# append/insert/extend/下标写入/列表(set/dict)字面量/实参/返回值：范围内
# 隐式、超范围编译报错（反例 f73-f76），与声明/赋值同口径，不静默窄化。
# 本用例取值都在宽度内，与 CPython 逐字节对拍（from __future__ import
# annotations；u64 大字面量在 CPython 里就是普通 int）。

l: list[i8] = []
l.append(5)
l.append(-128)
l.insert(0, 127)
l[0] = 3
print(l, sum(l))
m: list[i8] = [1, 2, 3]
print(m, sum(m))
m[1] = -5
m += [10, 20]
m.extend([30, 40])
print(m)
u: list[u8] = [200, 250]
u.append(255)
u.insert(1, 0)
print(u, sum(u))
w: list[u32] = [4294967295]
w.append(0)
print(w)
big: list[u64] = [18446744073709551615, 9223372036854775808]
big.append(18446744073709551614)
big[0] = 5
print(big, big[0] + big[1], len(big))
d: dict[str, i8] = {"a": 1}
d["b"] = 127
print(d["a"], d["b"])
dk: dict[i8, str] = {1: "one"}
dk[2] = "two"
print(dk[1], dk[2])
s: set[u8] = {1, 2}
s.add(255)
print(sorted(s))


def f(xs: list[i8]) -> int:
    return sum(xs)


def g() -> list[u8]:
    return [200, 255]


print(f([1, 2, 3]), g())
# 默认 int 容器与开放列表的既有行为不受影响
n: list[int] = [1, 2]
n.append(300)
n.insert(0, -99999)
n[0] = 7
print(n)
e = []
e.append(5)
e.insert(0, "x")
print(e)
# --- expect ---
# [3, 5, -128] -120
# [1, 2, 3] 6
# [1, -5, 3, 10, 20, 30, 40]
# [200, 0, 250, 255] 705
# [4294967295, 0]
# [5, 9223372036854775808, 18446744073709551614] 9223372036854775813 3
# 1 127
# one two
# [1, 2, 255]
# 6 [200, 255]
# [7, 1, 2, 300]
# ['x', 5]
