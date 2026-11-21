# 异常路径的局部堆值回收（回归：内联体槽位在 raise 后无人释放）
# 根因：IR 内联把 callee 的局部槽搬进调用方帧，但调用方的 release_slots 是
# 降级期（内联之前）按当时的槽表生成的，看不到后加的槽；异常被本帧接住时
# 也不触发帧展开（当前帧的槽留给函数自己退出时释放）。于是内联体局部槽里
# 的最后一个值没人 dec。str/bytes 是索引叶子（不进 GC 堆链、退出清扫收不
# 到），LSan 直接报 Direct leak；容器因退出清扫能兜住，所以只有叶子露出来。
# 修法见 src/opt_inline.cpp 的 release_inlined_slots_at_returns。
# 用 ASan 跑：
#   build/pyadamas build --asan -O2 -o q x.py
#   ASAN_OPTIONS=detect_leaks=1 ./q      # 退出码 0 且无 leak 报告

def raises_with_local(n: int) -> int:
    s: str = "x" + str(n)
    if n >= 0:
        raise ValueError("boom" + s)
    return len(s)

def catches_in_same_frame() -> int:
    total = 0
    for i in range(4):
        try:
            raises_with_local(i)
        except ValueError:
            total += 1
    return total

print(catches_in_same_frame())

def local_then_return(n: int) -> str:
    s: str = "a" + str(n)
    return s

print(local_then_return(7))

# 异常对象/消息串/临时栈仍要对称：抛出后值仍可用、cause 链正常
def chain() -> int:
    try:
        raises_with_local(1)
    except ValueError as e:
        raise KeyError("wrapped:" + str(e)) from e
    return 0

try:
    chain()
except KeyError as e:
    print("caught", str(e))

# 容器局部（非叶子）同路径：退出清扫能兜住，但这里一并钉住语义
def dict_local() -> int:
    d = {"k": 1}
    try:
        raise ValueError("d")
    except ValueError:
        return d["k"]
    finally:
        d["k"] = 0

print(dict_local())

# ── 第二轮加固：非 str 叶子的同类路径（class 实例 / list+dict / bytes）──
# 这些对象 kind 是 OBJ/LIST/DICT，**在 GC 堆链上**：entry 函数出口的
# pa_gc_shutdown 会不管 rc 直接 free 它们，所以「补释放」必须发生在收尾
# 序列之前（否则 dec 到已清扫对象：ASan heap-use-after-free / -O2 段错误）。
class Node:
    def __init__(self, n: int):
        self.s = "v" + str(n)
        self.n = n

def raises_with_instance() -> int:
    nd = Node(7)
    raise ValueError("inst" + str(nd.n))

def raises_with_containers() -> int:
    xs = [1, 2, 3]
    d = {"k": 1}
    b: bytes = b"x" + b"y"
    raise ValueError("containers" + str(len(xs) + len(d) + len(b)))

def raises_in_generator() -> int:
    s: str = "a" + "b"
    yield 1
    raise ValueError("gen" + s)

def catch_all() -> int:
    n = 0
    try:
        raises_with_instance()
    except ValueError:
        n += 1
    try:
        raises_with_containers()
    except ValueError:
        n += 1
    it = raises_in_generator()
    next(it)
    try:
        next(it)
    except ValueError:
        n += 1
    return n

print(catch_all())

# --- expect ---
# 4
# a7
# caught wrapped:boomx1
# 1
# 3
