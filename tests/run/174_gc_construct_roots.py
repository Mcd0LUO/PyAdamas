# GC「构造期对象不在根上」压力用例（针对 runtime/rt_*.c 的 pa_gc_pin 修复）。
#
# 背景：循环收集器的主收集只用「全局根表 / 临时值栈 / 帧记录 / 寄存器」四处
# 做根。C 运行时函数里刚分配、还没交回调用方的对象只存在于 C 局部变量里，
# 若两次分配之间恰好触发主收集，它会被当垃圾清扫，函数拿到的就是悬空指针。
# 这类窗口平时跑不出来：要累计分配跨过 major 阈值（约 8MB 起）且正好落在
# 窗口内的那几次分配上。所以本用例用「大输入 + 构造期循环」把分配量堆到
# 几十 MB，让每个被修过的构造路径都撞上主收集：
#   str.split / split(sep) / splitlines、enumerate、zip、dict.items、
#   pathlib.Path.parts、re.findall、itertools.permutations、file.readlines、
#   % 格式化、Match.groups。
# 这些路径若在构造期少了根，后面会读到已被复用的堆内存：轻则长度/内容错，
# 重则 SIGSEGV（修复前这些段落各自都能复现 139）。
#
# 注意 unknown 索引：整程序没有 unknown 值时，字符串是 untracked 叶子，
# rc_alloc 不计入收集阈值，构造期压力就落不到点上。模块里有生成器 ⇒
# 编译期打开存活索引 ⇒ 字符串进 GC 堆。函数本身不调用，只用来逼出索引。


def _force_unknown_index() -> int:
    yield 1


def stress_split() -> int:
    # split() 按空白：循环里逐个子串 str_new，结果列表只在 C 局部变量里。
    ws: str = "ab " * 120000
    parts: list = ws.split()
    a: int = len(parts) + len(parts[0]) + len(parts[119999])
    # split(sep)：同样的构造期循环，额外覆盖 maxsplit=0 的分支。
    comma: str = "ab," * 120000
    cp: list = comma.split(",")
    b: int = len(cp) + len(cp[120000])
    # splitlines：每个换行点一次分配。
    lines: str = "ab\n" * 120000
    ln: list = lines.splitlines()
    c: int = len(ln)
    return a + b + c


def stress_pairs() -> int:
    # enumerate / zip：外层列表 + 循环里逐个 pair 列表。
    xs: list[int] = list(range(120000))
    en: list = list(enumerate(xs))
    a: int = len(en) + en[119999][0] + en[119999][1]
    zs: list = list(zip(xs, xs))
    b: int = len(zs) + zs[119999][1]
    return a + b


def stress_dict_items() -> int:
    # dict.items()：循环里每个 (key,value) 元组一次分配。
    d: dict[str, int] = {}
    i: int = 0
    while i < 90000:
        d["k" + str(i)] = i
        i += 1
    items: list = d.items()
    return len(items) + items[0][1]


def stress_path_parts() -> int:
    # Path.parts：每个路径段一个字符串。
    from pathlib import Path
    p = Path("/" + "a/" * 100000)
    return len(p.parts)


def stress_refindall() -> int:
    # re.findall：每个匹配一个字符串，追加进 C 局部列表。
    import re
    found: list = re.findall(r"ab", "ab " * 150000)
    return len(found)


def stress_permutations() -> int:
    # itertools.permutations：递归里每个排列一个元组（约 35MB 分配）。
    from itertools import permutations
    xs: list[int] = list(range(9))
    perm: list = list(permutations(xs))
    return len(perm)


def stress_readlines() -> int:
    # file.readlines：每行一次 str_new。
    path: str = "/tmp/pyadamas_174_lines.txt"
    f = open(path, "w")
    i: int = 0
    while i < 100000:
        f.write("line\n")
        i += 1
    f.close()
    g = open(path, "r")
    rows: list = g.readlines()
    n: int = len(rows) + len(rows[0])
    g.close()
    return n


def stress_percent() -> int:
    # % 格式化：每次迭代若干临时串（pa_str_from_i64 -> fmt_pad）。
    total: int = 0
    i: int = 0
    while i < 150000:
        s: str = "%d-%s" % (i, "xy")
        total += len(s)
        i += 1
    return total


def stress_groups() -> int:
    # Match.groups()：每个捕获组一个字符串，追加进列表。
    import re
    rx = re.compile(r"(a)(b)(c)(d)(e)")
    total: int = 0
    i: int = 0
    while i < 100000:
        m = rx.search("xxabcdeyy")
        if m is not None:
            gs = m.groups()
            total += len(gs)
        i += 1
    return total


print(stress_split())
print(stress_pairs())
print(stress_dict_items())
print(stress_path_parts())
print(stress_refindall())
print(stress_permutations())
print(stress_readlines())
print(stress_percent())
print(stress_groups())

# --- expect ---
# 360005
# 599997
# 90000
# 100001
# 150000
# 362880
# 100005
# 1238890
# 500000
