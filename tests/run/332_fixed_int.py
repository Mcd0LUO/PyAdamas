# 定宽整数：基本运算、回绕、控制流
# 0.382 合并自 3 个文件：332_fixed_int_basic.py, 333_fixed_int_wrap.py, 335_fixed_int_control.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 332_fixed_int_basic.py ──
def _case_1() -> None:
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

# ── 原 333_fixed_int_wrap.py ──
def _case_2() -> None:
    # 333_fixed_int_wrap —— P1 定宽整数的回绕/符号语义（**pa 语义**，不是 CPython）
    #
    # 定宽是 C 式二进制补码回绕，不是 CPython 的无限精度：
    #   i8 127+1 → -128，u8 255+1 → 0，i32 2^31-1 + 1 → -2^31，u64 2^64-1 + 1 → 0。
    # 同宽 signed+unsigned → unsigned（C 口径）：i8(-1) 与 u8 比较时按 255。
    # 宽度 < 64 的无符号值在槽内恒非负（零扩展不变量），所以 signed 指令与
    # 无符号指令在这些宽度上逐例等价；只有 u64 需要 divq/shrq/jb-ja。
    # 每个值都是手算的 pa 期望（需要无限精度请用 P3 的 big，不在本用例范围）。

    a: i8 = 127
    b: i8 = 1
    print(a + b)
    print(cast_i8(-128) - cast_i8(1))
    print(cast_i8(100) * cast_i8(3))
    print(cast_i8(-128) // cast_i8(-1))
    print(cast_i8(-128) % cast_i8(-1))
    print(-cast_i8(-128))
    print(~cast_i8(0))

    u: u8 = 255
    print(u + cast_u8(1))
    print(u - cast_u8(1))
    print(cast_u8(200) + cast_u8(100))
    print(cast_u8(255) * cast_u8(2))
    u2: u8 = 200
    u2 += 100
    print(u2)
    u3: u8 = 0
    u3 -= 1
    print(u3)

    s: i16 = 32767
    print(s + cast_i16(1))
    print(cast_i16(-32768) - cast_i16(1))
    print(cast_u16(65535) + cast_u16(1))
    print(cast_i32(2147483647) + cast_i32(1))
    print(cast_u32(4294967295) + cast_u32(1))
    print(cast_u32(4294967295) // cast_u32(2))
    print(cast_u32(4294967295) % cast_u32(2))
    print(9223372036854775807 + 1)
    print(cast_u64(-1) + cast_u64(1))
    print(cast_u64(-1) // cast_u64(2))
    print(cast_u64(-1) % cast_u64(3))
    print(cast_u64(-2) - cast_u64(3))
    print(cast_u64(-1) > cast_u64(1))
    print(cast_u64(-3) // cast_u64(5), cast_u64(-3) % cast_u64(5))

    print(cast_i8(-1) < cast_u8(1))
    print(cast_i8(-1) == cast_u8(255))
    print(cast_i32(-1) + cast_u32(2))
    print(cast_i32(-1) + cast_u32(1))
    print(cast_i16(-1) > cast_u16(1))

    print(cast_u8(1) << cast_u8(8))
    print(cast_i8(-128) >> cast_i8(1))
    print(cast_u8(128) >> cast_u8(1))
    print(cast_i8(1) << cast_i8(7))
    print(cast_u32(2147483648) >> cast_u32(1))

    print(cast_i8(200))
    print(cast_i8(-200))
    print(cast_u8(-1))
    print(cast_i16(65535))
    print(cast_u16(-1))
    print(cast_i32(2147483648))
    print(cast_u32(-1))
    print(cast_u64(-1))
    print(cast_i64(cast_u32(-1)))
    print(cast_u8(cast_i32(300)))
    print(abs(cast_i8(-128)))
    print(cast_i16(-7) // cast_i16(3), cast_i16(-7) % cast_i16(3))
    print(cast_u8(200) // cast_u8(3), cast_u8(200) % cast_u8(3))
    print(cast_i8(-7) // cast_i8(3), cast_i8(-7) % cast_i8(3))
    z: i32 = 2000000000
    print(z * cast_i32(2))
    w: i8 = 100
    w += 10
    w += 10
    w += 10
    print(w)

# ── 原 335_fixed_int_control.py ──
def _case_3() -> None:
    # 335_fixed_int_control —— P1 定宽整数在控制流/循环/下标里的符号语义
    #
    # 比较分支（codegen 走 jcc 而不是 setcc）与循环条件必须按提升后的类型
    # 选 signed/unsigned：u64 用 jb/ja，其余宽度符号指令等价（值非负不变量）。
    # 这里覆盖 if/while/for、负值分支、无符号边界比较、定宽循环计数（回绕退出）。

    i: i8 = -5
    if i < 0:
        print("neg")
    else:
        print("nonneg")
    u: u8 = 200
    if u > 100:
        print("big")
    else:
        print("small")
    n: u64 = cast_u64(-1)
    if n > cast_u64(1):
        print("u64-big")
    if n < cast_u64(1):
        print("u64-small")
    if cast_u32(4294967295) > cast_u32(1):
        print("u32-big")
    if cast_i32(-1) < cast_i32(1):
        print("i32-neg")

    total: i32 = 0
    k: i8 = 0
    while k < 5:
        total += k
        k += 1
    print(total, k)

    count: u8 = 0
    while count < cast_u8(200):
        count += cast_u8(50)
    print(count)

    dec: i16 = 0
    while True:
        dec -= 1
        if dec < cast_i16(-3):
            break
    print(dec)

    neg_u8: u8 = 1
    print(-neg_u8)
    print(~neg_u8)

    sq: i8 = 0
    for v in range(1, 100):
        sq = cast_i8(sq + cast_i8(v * v % 100))
        if sq > cast_i8(50):
            break
    print(sq)

    lst: list[int] = [1, 2, 3, 4]
    idx: u8 = 3
    print(lst[idx])
    print(lst[idx] + lst[cast_i8(0)])

    print(min(cast_u64(-1), cast_u64(0)))
    print(max(cast_i64(-9223372036854775807 - 1), cast_i64(9223372036854775807)))

_case_1()
_case_2()
_case_3()

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
# -128
# 127
# 44
# -128
# 0
# -128
# -1
# 0
# 254
# 44
# 254
# 44
# 255
# -32768
# 32767
# 0
# -2147483648
# 0
# 2147483647
# 1
# -9223372036854775808
# 0
# 9223372036854775807
# 0
# 18446744073709551611
# True
# 3689348814741910322 3
# False
# True
# 1
# 0
# True
# 0
# -64
# 64
# -128
# 1073741824
# -56
# 56
# 255
# -1
# 65535
# -2147483648
# 4294967295
# 18446744073709551615
# 4294967295
# 44
# 128
# -3 2
# 66 2
# -3 2
# -294967296
# -126
# neg
# big
# u64-big
# u32-big
# i32-neg
# 10 5
# 200
# -4
# 255
# 254
# 55
# 4
# 5
# 0
# 9223372036854775807
