# 非 ASCII 码点综合：len、索引、驻留、方法
# 0.382 合并自 4 个文件：176_codepoint_len.py, 177_codepoint_index.py, 178_codepoint_intern.py, 179_codepoint_methods.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 176_codepoint_len.py ──
def _case_1() -> None:
    # 码点长度语义（S1）：len 按码点、len_bytes 按字节、bytes 完全不变
    s = "中文"
    print(len(s))
    print(len("abc"))
    print(len(""))
    print(len("中"))
    print(len("a中b"))
    print(len("😀"))
    print(len("中文abc"))

    # 字节长度出口
    print(len_bytes(s))
    print(len_bytes("abc"))
    print(len_bytes(""))
    print(len_bytes("😀"))

    # bytes 语义完全不变：len 就是字节数
    print(len(b"中文"))
    print(len(b"abc"))
    print(len_bytes(b"abc"))

    # 字面量 vs 运行时拼接
    lit = "中文"
    cat = "中" + "文"
    print(len(lit), len(cat), lit == cat)

    # 从 ASCII 片段拼出非 ASCII
    parts = "a" + "中" + "b"
    print(len(parts), len_bytes(parts))

    # 长串（超过 SSO 阈值 7 字节）
    long = "中文" * 100
    print(len(long), len_bytes(long))
    print(len(long) * 3 == len_bytes(long))

    # 空串/单字符边界
    print(len(" "), len_bytes(" "))
    print(len("\n"), len_bytes("\n"))

    # 4 字节 emoji 混合
    mix = "a😀b中c"
    print(len(mix), len_bytes(mix))
    print(mix[0], mix[1], mix[2], mix[3], mix[4])

# ── 原 177_codepoint_index.py ──
def _case_2() -> None:
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

# ── 原 178_codepoint_intern.py ──
def _case_3() -> None:
    # 字面量 intern 按 (kind, 内容) 去重 + 字面量 vs 运行时拼接 + 混合 ASCII
    # "中" 与 b"\xe4\xb8\xad" 字节相同但 pub 不同，必须各自持有正确的长度。

    # 1) 同一内容同时以 str / bytes 出现
    s1 = "中"
    b1 = b"\xe4\xb8\xad"
    print(len(s1), len_bytes(s1))
    print(len(b1), len_bytes(b1))
    print(len("中"), len(b"\xe4\xb8\xad"))

    # 2) 同一 str 内容重复出现（应复用同一个静态对象）
    a = "重复的字面量"
    c = "重复的字面量"
    print(a == c, len(a), len_bytes(a))

    # 3) 字面量 vs 运行时拼接：长度必须一致
    lit = "中文测试"
    cat = "中" + "文" + "测试"
    print(len(lit), len(cat), lit == cat, len_bytes(lit), len_bytes(cat))

    # 4) 运行时构造的串（非字面量）也要有正确 pub
    buf = ""
    for ch in "abc":
        buf = buf + ch
    print(len(buf), len_bytes(buf))
    cn = ""
    for ch in "中文":
        cn = cn + ch
    print(len(cn), len_bytes(cn), cn)

    # 5) 混合 ASCII/非 ASCII 的各种构造
    x = "a" + "中" + "b" + "文" + "c"
    print(len(x), len_bytes(x))
    print(x[0], x[1], x[2], x[3], x[4])

    # 6) 4 字节 emoji（字面量里直接写 UTF-8）
    e = "😀"
    print(len(e), len_bytes(e), ord(e))
    e2 = "a" + e + "b"
    print(len(e2), len_bytes(e2))
    print(e2[0], e2[1], e2[2])
    print(e * 2, len(e * 2), len_bytes(e * 2))

    # 7) str/bytes 转换：encode/decode 后长度口径切换
    print(len("中文".encode()), len_bytes("中文".encode()))
    print(len(b"\xe4\xb8\xad".decode()), len_bytes(b"\xe4\xb8\xad".decode()))
    print(len("abc".encode()), len("abc".encode()))

    # 8) 从容器里取出的串同样正确
    lst = ["中", "ab", "😀"]
    for item in lst:
        print(len(item), len_bytes(item))

# ── 原 179_codepoint_methods.py ──
def _case_4() -> None:
    # 字符串方法在码点语义下的关键行为（S3 未做的方法只测不破坏的部分）
    s = "中文abc"
    print(len(s), len_bytes(s))

    # find/rfind 返回码点下标
    print(s.find("abc"))
    print(s.find("文"))
    print(s.rfind("a"))
    print(s.find("zz"))
    print(s.find("文", 1))
    print(s.find("a", 3))

    # count / startswith / endswith / in
    print(s.count("中"), s.count("zz"), s.count(""))
    print(s.startswith("中"), s.endswith("bc"))
    print("文" in s, "x" in s)

    # 切片 + 方法组合
    print(s[2:], s[:2], s[1:3])

    # strip（chars 按码点集合匹配）
    print("中中文中".strip("中"))
    print("  中文  ".strip())

    # split / join
    print("中,文,a".split(","))
    print(",".join(["中", "文"]))
    print(len(",".join(["中", "文"])))

    # replace（空 old 必须按码点插入，不能切开多字节序列）
    print("中文".replace("", "-"))
    print(len("中文".replace("", "-")))
    print("中文".replace("中", "X"))

    # upper/lower 只动 ASCII，pub 不变
    print("中a文".upper(), len("中a文".upper()))
    print("中A文".lower(), len("中A文".lower()))

    # capitalize / title 不破坏多字节序列
    print("中a文".capitalize())
    print("中a文 b".title())  # 非 ASCII 按非字母：已知差异

    # 宽度方法按码点
    print("中".ljust(3, "-"))
    print("中".rjust(3, "-"))
    print("中".center(5, "-"))
    print("中".zfill(3))
    print(len("中".ljust(3, "-")))

    # removeprefix / removesuffix
    print("中文abc".removeprefix("中"))
    print("中文abc".removesuffix("bc"))
    print(len("中文abc".removeprefix("中")))

    # 比较/哈希/字典 key
    print("中" < "文", "a" < "中")
    d = {"中": 1, "文": 2}
    print(d["中"], d["文"])
    print(hash("中") == hash("中"))

    # ord/chr 按码点
    print(ord("中"), chr(20013), len(chr(20013)))
    print(ord("😀"), chr(128512))

    # repr 与 % 格式化
    print(repr("中"))
    print("%s" % "中")
    print("%s-%s" % ("中", "文"))

_case_1()
_case_2()
_case_3()
_case_4()

# --- expect ---
# 2
# 3
# 0
# 1
# 3
# 1
# 5
# 6
# 3
# 0
# 4
# 6
# 3
# 3
# 2 2 True
# 3 5
# 200 600
# True
# 1 1
# 1 1
# 5 10
# a 😀 b 中 c
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
# 1 3
# 3 3
# 1 3
# True 6 18
# 4 4 True 12 12
# 3 3
# 2 6 中文
# 5 9
# a 中 b 文 c
# 1 4 128512
# 3 6
# a 😀 b
# 😀😀 2 8
# 6 6
# 1 3
# 3 3
# 1 3
# 2 2
# 1 4
# 5 9
# 2
# 1
# 2
# -1
# 1
# -1
# 1 0 6
# True True
# True False
# abc 中文 文a
# 文
# 中文
# ['中', '文', 'a']
# 中,文
# 3
# -中-文-
# 5
# X文
# 中A文 3
# 中a文 3
# 中a文
# 中a文 B
# 中--
# --中
# --中--
# 00中
# 3
# 文abc
# 中文a
# 4
# True True
# 1 2
# True
# 20013 中 1
# 128512 😀
# '中'
# 中
# 中-文
