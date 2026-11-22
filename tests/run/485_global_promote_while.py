# promote_globals_in_loops：while 循环的全局提升（修复「body→exit 恰一条边」误拒）。
#
# 原判据只数 body→exit 的边，而 while/for 的出口边都在 header 上（body 只回边到
# header）→ exits != 1 → 所有 while 循环的全局提升都被误拒。修复后：出口边 =
# header→exit + body→exit 合计恰好 1 条；exit 只能从循环内进入；body 不能被循环外
# 路径直接进入；循环内不能有调用；非入口函数还要求循环内没有可抛异常指令；
# 带 EH 标记的函数整段跳过。

gcount = 0
gi = 0
gtouch = 3


def bump() -> None:
    # 循环内调用会改全局：必须保守不提升。
    global gcount
    gcount = gcount + 10


def loop_while_global(n: int) -> int:
    # 无调用、无抛异常指令、单一出口 → 提升到槽，出口写回。
    global gcount
    global gi
    gcount = 0
    gi = 0
    while gi < n:
        gcount = gcount + 1
        gi = gi + 1
    return gcount


def loop_with_call(n: int) -> int:
    # 循环体内有调用：能内联时会变成直接写全局（提升仍正确）；不能内联时
    # has_call 判据保守拒绝提升（见下面的 loop_with_rtcall）。
    global gcount
    gcount = 0
    i = 0
    while i < n:
        bump()
        i = i + 1
    return gcount


class Counter:
    def bump(self) -> None:
        global gcount
        gcount = gcount + 1


def loop_with_vcall(n: int) -> int:
    # 动态方法调用：devirt+内联后同样提升正确（测试数值仍必须对）。
    global gcount
    gcount = 0
    c = Counter()
    i = 0
    while i < n:
        c.bump()
        i = i + 1
    return gcount


def nested_while_global(n: int, m: int) -> int:
    # 嵌套 while，内层读写全局 → 提升/写回顺序必须正确。
    global gcount
    gcount = 0
    i = 0
    while i < n:
        j = 0
        while j < m:
            gcount = gcount + 1
            j = j + 1
        i = i + 1
    return gcount


def read_only_global(n: int) -> int:
    # 循环内只读全局（不改）→ 可缓存到槽。
    s = 0
    i = 0
    while i < n:
        s = s + gtouch
        i = i + 1
    return s


def index_loop(gl: list, n: int) -> int:
    # 非入口函数 + 循环内有检查型下标（可能 IndexError）→ 不提升。
    global gcount
    gcount = 0
    i = 0
    while i < n:
        gcount = gcount + gl[i]
        i = i + 1
    return gcount


gl = [1, 2, 3, 4]
gcount = 0
gi = 0
print("while_global0", loop_while_global(0), gcount)
print("while_global1", loop_while_global(1), gcount)
print("while_global3", loop_while_global(3), gcount)
print("with_call", loop_with_call(3), gcount)
print("with_vcall", loop_with_vcall(3), gcount)


def loop_with_rtcall(n: int) -> int:
    # 运行时调用（str()，CallRt，不可内联）→ 循环内 has_call → 不提升。
    global gcount
    gcount = 0
    i = 0
    while i < n:
        s = str(i)
        if len(s) > 0:
            gcount = gcount + 1
        i = i + 1
    return gcount


print("with_rtcall", loop_with_rtcall(3), gcount)
print("nested", nested_while_global(2, 3), gcount)
print("read_only", read_only_global(4))
print("index_loop", index_loop(gl, 4))
# 模块级 while（入口 __pa_main）：像 sieve 一样「循环体里有检查型下标」也允许提升，
# 因为入口函数的未捕获异常 = 进程终止，推迟写回不可观察。
top = 0
k = 0
while k < 4:
    top = top + gl[k]
    k = k + 1
print("top_level", top, k)

# --- expect ---
# while_global0 0 0
# while_global1 1 1
# while_global3 3 3
# with_call 30 30
# with_vcall 3 3
# with_rtcall 3 3
# nested 6 6
# read_only 12
# index_loop 10
# top_level 10 4
