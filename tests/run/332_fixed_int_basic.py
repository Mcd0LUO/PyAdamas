# 332_fixed_int_basic —— P1 定宽整数：标注/提升/转换（取值不溢出宽度，
# 本用例的输出可与 CPython 逐字节对拍：定宽整数在 Python 语义里仍是 int）。
#
# 覆盖：i8/i16/i32/i64(long/int)、u8/u16/u32、char=i8、short=i16 别名；
# §1.2 提升（含两侧转换）；同一表达式里混宽/混符号（值不越界时结论与
# CPython 一致）；函数形参/默认值/返回值；容器元素；类字段。

a: i8 = 7
b: i8 = 3
print(a + b, b - a, a * b, a // b, a % b, -a, ~b)
print(a << 2, a >> 1)
c: i16 = 300
print(b + c, c - a)
d: i32 = 7000
print(a * d, d + c, d // a, d % a)
u: u8 = 7
v: u8 = 3
print(u + v, u * v, u // v, u % v, u - v)
print(u + 1, 1 + u)
print(a + u)
n: int = 100
print(a + n, n + a, n // a)
lg: long = 5
print(lg + a)
ch: char = 65
print(ch + 1)
sh: short = -5
print(sh + 1, sh * 2)
t: bool = True
print(t + b, t + u)
f: float = 1.5
print(a + f, f + a)
print(a / b)
print(a > b, a < b, a == b, a != b)
print(u > v, u >= v, u < v, u <= v)
print(a == n)
print(type(a) == int, isinstance(a, int), isinstance(a, bool))
print(cast_i32(a) == a, cast_i16(a) == a, cast_u8(a) == a)
print(cast_i64(a) == a, cast_u64(a) == a)


def scaled(x: i8, k: i16 = 2) -> i32:
    return cast_i32(x * k)


def widen(x: u8) -> int:
    return x + 0


print(scaled(5))
print(scaled(5, -3))
print(widen(u))
print(widen(cast_u8(200)))

vals: list[i8] = [cast_i8(1), cast_i8(2), cast_i8(3)]
print(vals)
print(sum(vals))
print(sorted(vals, reverse=True))
print(max(vals), min(vals))
keys: dict[u8, str] = {cast_u8(1): "one"}
print(keys[cast_u8(1)])
print("one" in keys.values())


class Counter:
    def __init__(self):
        self.n: i8 = 0
        self.total: i32 = 0

    def bump(self) -> i8:
        self.n += 1
        self.total += 3
        return self.n


ctr: Counter = Counter()
print(ctr.bump(), ctr.bump(), ctr.total)
ctr.n = 5
print(ctr.n)
print(f"{a}", f"{a:03d}", format(u, "x"))
print("%d-%d" % (a, u))
print(hex(u), bin(u), oct(u))
# --- expect ---
# 10 -4 21 2 1 -7 -4
# 28 3
# 303 293
# 49000 7300 1000 0
# 10 21 2 1 4
# 8 8
# 14
# 107 107 14
# 12
# 66
# -4 -10
# 4 8
# 8.5 8.5
# 2.3333333333333335
# True False False True
# True True False False
# False
# True True False
# True True True
# True True
# 10
# -15
# 7
# 200
# [1, 2, 3]
# 6
# [3, 2, 1]
# 3 1
# one
# True
# 1 2 6
# 5
# 7 007 7
# 7-7
# 0x7 0b111 0o7
