# 异常循环 10 万次：raise 构造的异常值、except as e 绑定的异常值都会在后续
# 分配时经历 major GC。GC 的根集合只认「全局根表 / 临时值栈 / 帧记录 / 寄存器」，
# 于是这里钉住两类曾经的 use-after-free：
#   1) raise 与 accept 之间挂在运行时全局上的多参 args 表没登记成根，被主
#      收集当垃圾清扫，accept 时对已复用内存做 rc_inc（SIGSEGV，rc=139）；
#   2) 异常值构造过程中还会继续分配（args 列表、消息串、cause 递归），刚分配
#      还没交回调用方的半成品不在任何根上，被主收集清扫成悬空指针。
# 两者都要规模够大、触发 major GC 才出现；2 万次以内看不出问题。
def make(i: int) -> int:
    text: str = "ababab"
    if i % 2 == 0:
        raise ValueError(text)
    return len(text)


def deep(i: int) -> int:
    payload: list[int] = list(range(i % 3 + 2))
    text: str = "ab" * (i % 4 + 1)
    if i % 5 == 0:
        raise ValueError(text)
    return len(payload) + len(text)


i: int = 0
raised: int = 0
returned: int = 0
checksum: int = 0
while i < 100000:
    try:
        checksum += make(i)
        returned += 1
    except ValueError as e:
        raised += 1
        checksum += len(e.args)
    i += 1
print(raised, returned, checksum)

j: int = 0
caught: int = 0
total: int = 0
while j < 1000:
    try:
        total += deep(j)
    except ValueError:
        caught += 1
    j += 1
print(caught, total)

# --- expect ---
# 50000 50000 350000
# 200 6399
