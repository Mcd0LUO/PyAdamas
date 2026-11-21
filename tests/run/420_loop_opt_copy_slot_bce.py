# pyadamas: loop-opt
# 整数循环仿射 BCE（PYADAMAS_LOOP_OPT）的 IV 副本槽判据回归。
#
# 旧判据只要求「副本槽的写块支配候选块」，不看同块内的指令位置：
#   for i in range(10):
#       a[j] = i     # 这里的 j 还是循环外的旧值
#       j = i
# store(j) 与 load(j) 同块但写在后，块级支配恒真 —— 守卫按本轮 IV 证明
# 在界内，克隆循环却按旧 j 做 unchecked 写（越界写 / SIGSEGV / 负下标写
# 到堆前面）。现在要求副本槽的写必须支配读到它的那条 load：跨块比支配，
# 同块必须写在前。
#
# 每个函数都留一条自递归边：被内联后主函数里的那份不会被版本化，
# 门控一旦真的出错就抓不到。
def stale_index(a: list[int], n: int) -> int:
    if n < 0:
        return stale_index(a, n + 1)
    j: int = 1000
    s: int = 0
    for i in range(10):
        a[j] = i
        j = i
        s = s + i
    return s


def stale_while(a: list[int], n: int) -> int:
    if n < 0:
        return stale_while(a, n + 1)
    j: int = 999
    i: int = 0
    s: int = 0
    while i < 10:
        a[j] = i
        j = i
        s = s + i
        i = i + 1
    return s


def stale_negative(a: list[int], n: int) -> int:
    if n < 0:
        return stale_negative(a, n + 1)
    j: int = -1
    s: int = 0
    for i in range(6):
        a[j] = 100 + i
        j = i
        s = s + 1
    return s


def write_first(a: list[int], n: int) -> int:
    if n < 0:
        return write_first(a, n + 1)
    j: int = 7
    s: int = 0
    for i in range(8):
        j = i
        a[j] = a[j] + 3
        s = s + a[j]
    return s


def two_copies(a: list[int], n: int) -> int:
    if n < 0:
        return two_copies(a, n + 1)
    s: int = 0
    for i in range(8):
        k: int = i
        j: int = k
        a[j] = a[j] + 2
        s = s + a[j]
    return s


def stale_in_range(a: list[int], n: int) -> int:
    if n < 0:
        return stale_in_range(a, n + 1)
    j: int = 3
    s: int = 0
    for i in range(5):
        a[j] = j * 10 + i
        j = i
        s = s + 1
    return s


def run(name: str, fn: object, a: list[int]) -> None:
    # 先求值再 print：PyAdamas 的 print 是降级期展开的，实参边算边写
    # （docs/compatibility.md 的文档化差异），这里要让两边的输出可比。
    try:
        r: int = fn(a, 1)
        print(name, r)
    except IndexError:
        print(name, "IndexError")
    print(name, a)


a1: list[int] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
run("stale", stale_index, a1)
a2: list[int] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
run("while", stale_while, a2)
a3: list[int] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
run("negative", stale_negative, a3)
a4: list[int] = [1, 2, 3, 4, 5, 6, 7, 8]
run("writefirst", write_first, a4)
a5: list[int] = [1, 2, 3, 4, 5, 6, 7, 8]
run("twocopies", two_copies, a5)
a6: list[int] = [0, 0, 0, 0, 0]
run("inrange", stale_in_range, a6)

# --- expect ---
# stale IndexError
# stale [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
# while IndexError
# while [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
# negative 6
# negative [101, 102, 103, 104, 105, 0, 0, 0, 0, 100]
# writefirst 60
# writefirst [4, 5, 6, 7, 8, 9, 10, 11]
# twocopies 52
# twocopies [3, 4, 5, 6, 7, 8, 9, 10]
# inrange 5
# inrange [1, 12, 23, 34, 0]
