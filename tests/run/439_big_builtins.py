# big 与内置/容器交互：sum、min/max、容器元素、% 格式化
# 0.382 合并自 4 个文件：439_sum_big.py, 440_minmax_big.py, 441_big_containers.py, 446_percent_big.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 439_sum_big.py ──
def _case_1() -> None:
    # 439: sum() 支持 big 元素与 big 起始值（精确累加，不回绕）。
    # 降级侧原来在累加循环里发裸 Bin Add，而 big 加法是运行时函数 —— 于是
    # 即使放开类型检查也会把 big 指针当整数相加（实测段错误）。


    xs: list[big] = [1180591620717411303424, 1, 2]
    print(sum(xs))
    print(sum(xs, 10))

    empty: list[big] = []
    print(sum(empty))

    big_start: big = 2361183241434822606848
    print(sum(xs, big_start))

    # 普通整数与浮点路径不受影响
    ints: list[int] = [1, 2, 3]
    print(sum(ints))
    floats: list[float] = [1.5, 2.5]
    print(sum(floats))

# ── 原 440_minmax_big.py ──
def _case_2() -> None:
    # 440: min()/max() 支持 big（比较走运行时 pa_big_cmp）。
    # 裸 Cmp 的 codegen 没有 big 路径，会把 big 指针当整数比 —— 所以聚合里
    # 的比较统一收口到 take_new()：big 先 BigCmp 再与 0 比较。


    xs: list[big] = [1180591620717411303424, 5, 1180591620717411303425]
    print(max(xs))
    print(min(xs))

    print(max(1180591620717411303424, 5))
    print(min(5, 1180591620717411303424))
    print(max(1, 2, 3))
    print(min(3, 2, 1))

    neg: list[big] = [-1180591620717411303424, -5]
    print(max(neg))
    print(min(neg))

    # 普通路径不受影响
    ints: list[int] = [3, 1, 2]
    print(max(ints), min(ints))
    floats: list[float] = [1.5, 2.5]
    print(max(floats), min(floats))
    strs: list[str] = ["b", "a"]
    print(max(strs), min(strs))

# ── 原 441_big_containers.py ──
def _case_3() -> None:
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

# ── 原 446_percent_big.py ──
def _case_4() -> None:
    # 446: `%` 格式化支持 big（d/i/u/x/X/o 与 s/r）。
    # 描述符里之前没有 big 档，落 `x` → "unsupported format argument type"。
    # 现在用 B 档：整数转换交给 pa_big_fmt（与 format() 共用），
    # s/r 用十进制串。


    b: big = 1180591620717411303424
    n: big = -1180591620717411303424
    s: big = 5
    print("%d" % b)
    print("%x" % b)
    print("%o" % b)
    print("%10d|" % s)
    print("%-10d|" % s)
    print("%+d" % s)
    print("%05d" % s)
    print("%d" % n)
    print("%5d|" % n)
    print("%+d" % n)
    print("%d%%" % b)
    print("%s" % b)
    print("%r" % b)

_case_1()
_case_2()
_case_3()
_case_4()

# --- expect ---
# 1180591620717411303427
# 1180591620717411303437
# 0
# 3541774862152233910275
# 6
# 4.0
# 1180591620717411303425
# 5
# 1180591620717411303424
# 5
# 3
# 1
# -5
# -1180591620717411303424
# 3 1
# 2.5 1.5
# b a
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
# 1180591620717411303424
# 400000000000000000
# 200000000000000000000000
#          5|
# 5         |
# +5
# 00005
# -1180591620717411303424
# -1180591620717411303424|
# -1180591620717411303424
# 1180591620717411303424%
# 1180591620717411303424
# 1180591620717411303424
