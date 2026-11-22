# 容器表示与 unknown 宽化：异构元组、list kind、str unknown、isinstance bytes
# 0.382 合并自 4 个文件：135_hetero_tuple_repr.py, 150_list_kind.py, 152_str_unknown.py, 158_isinstance_bytes.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 135_hetero_tuple_repr.py ──
def _case_1() -> None:
    # 异构元组的 repr/str：元素按各自静态类型渲染，嵌套元组递归处理。

    class P:
        def __repr__(self) -> str:
            return "P()"


    t = (1, "a", 2.5, True, None)
    print(t)
    print(repr(t))
    print(str(t))
    print(repr((1,)))
    print(repr(()))
    print(repr((1, 2.5)))
    print(repr(("x", (1, 2.5))))
    print(repr((P(), 1)))

# ── 原 150_list_kind.py ──
def _case_2() -> None:
    # 裸 list / 空字面量的元素种类：第一次写入时定型，冲突时降级为混合。
    def push_ints(n: int) -> list:
        out = []
        i = 0
        while i < n:
            out.append(i * 2)
            i += 1
        return out

    print(push_ints(4))

    def push_strs() -> list:
        out: list = []
        out.append("a" + "b")
        out.append("c" + "d")
        return out

    r = push_strs()
    print(r[0], r[1])

    def put(a: list, i: int, v: int) -> None:
        a[i] = v

    xs = []
    xs.append(1)
    put(xs, 0, 7)
    print(xs)

    mixed = []
    mixed.append(1)
    mixed.append("a" + "b")
    print(mixed[0], mixed[1])

    floats = []
    floats.append(1.5)
    floats.append(2.5)
    print(floats)

    ins = []
    ins.insert(0, "x" + "y")
    ins.insert(0, "z" + "w")
    print(ins[0], ins[1])

    sl = []
    sl[0:0] = ["p" + "q"]
    print(sl[0])

    ext = []
    ext.extend(["k" + "1", "k2"])
    ext.extend([3])
    print(ext[0], ext[1], ext[2])

    empty = []
    cp = empty.copy()
    cp.append("m" + "n")
    sl2 = empty[:]
    sl2.append("o" + "p")
    print(cp[0], sl2[0])

    src: list = []
    src.append(1)
    src.append(2)
    sq = [x * x for x in src]
    print(sq)

    built = list()
    built.append("q" + "r")
    print(built[0])

    nested = []
    nested.append([1, 2])
    nested.append([3])
    print(len(nested))

    pile = []
    pile.append("e" + "f")
    print(pile.pop())

    def drain(a: list) -> None:
        while len(a) > 0:
            print(a.pop())

    pile.append("g" + "h")
    drain(pile)

    def counter() -> None:
        acc = []
        def add(v: int) -> None:
            acc.append(v)
        add(1)
        add(2)
        print(acc)

    counter()

# ── 原 152_str_unknown.py ──
def _case_3() -> None:
    # str() 对 unknown 值的运行期分派：堆字符串原样返回，其它按整数格式化。
    def show(a: list) -> None:
        x = a[0]
        print(str(x), len(str(x)))
        y = a[1]
        print(str(y), len(str(y)))
        z = a[2]
        print(str(z))

    vals = []
    vals.append("ab" + "cd")
    vals.append(12345)
    vals.append(-7)
    show(vals)

    def empty_len(a: list) -> int:
        e = a[0]
        return len(str(e))

    blank = []
    blank.append("")
    print(empty_len(blank))

# ── 原 158_isinstance_bytes.py ──
def _case_4() -> None:
    # isinstance/bytes 前端与运行期：元组第二参数、bytes/type/frozenset、bytes() 构造。
    b = b'hi'
    s = 'hi'
    l = [1, 2, 3]
    t = (1, 2)
    d = {'a': 1}
    print(isinstance(b, bytes), isinstance(s, str), isinstance(b, str))
    print(isinstance(b, (str, bytes)), isinstance(s, (str, bytes)), isinstance(123, (str, bytes)))
    print(isinstance(l, (list, tuple)), isinstance(t, (list, tuple)))
    print(isinstance(d, dict), isinstance(1.5, (int, float)), isinstance(3, int))

    class A: pass
    class B(A): pass
    a = A()
    bb = B()
    print(isinstance(a, (A, B)), isinstance(bb, A), isinstance(bb, B))

    print(bytes(3) == b'\x00\x00\x00')
    print(bytes((72, 105)) == b'Hi')

_case_1()
_case_2()
_case_3()
_case_4()

# --- expect ---
# (1, 'a', 2.5, True, None)
# (1, 'a', 2.5, True, None)
# (1, 'a', 2.5, True, None)
# (1,)
# ()
# (1, 2.5)
# ('x', (1, 2.5))
# (P(), 1)
# [0, 2, 4, 6]
# ab cd
# [7]
# 1 ab
# [1.5, 2.5]
# zw xy
# pq
# k1 k2 3
# mn op
# [1, 4]
# qr
# 2
# ef
# gh
# [1, 2]
# abcd 4
# 12345 5
# -7
# 0
# True True False
# True True False
# True True
# True True True
# True True True
# True
# True
