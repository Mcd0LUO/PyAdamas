# 331_type_tags_rc —— P2：unknown 槽/容器在类型标签下的 RC 正确性
# pyadamas: type-tags
#
# 覆盖：堆元素容器（tuple/set/bytes）、标量元素容器（bool/None/float/int）
# 各自定型（避免 0.339 就存在的「kind1→kind2 中途转混合不建位图 → 未跟踪
# 元组漏 dec」旧缺口，另见报告边界）、unknown 槽反复改写不同类型（每轮
# inc 新值 / dec 旧值）、同模块直调带 tag 的参数与返回值（tuple/set/bytes
# 过 unknown 不再退化成指针/0）。ASan detect_leaks=1 钉子：无泄漏、无 UAF。
def take(x: object) -> object:
    return x

def main() -> None:
    heapy: list[object] = []
    scalars: list[object] = []
    i: int = 0
    while i < 200:
        heapy.append((i, i + 1))
        heapy.append({i})
        heapy.append(b"xy")
        scalars.append(True)
        scalars.append(None)
        scalars.append(2.5)
        scalars.append(i)
        i += 1
    print(len(heapy), len(scalars))
    t: object = None
    t = (1, 2)
    t = {3}
    t = b"z"
    t = True
    t = 1
    t = 2.5
    t = "s"
    t = [1, 2]
    print(repr(t))
    u: object = take((7, 8))
    print(repr(u))
    v: object = take({9})
    print(repr(v))
    w: object = take(b"q")
    print(repr(w))

main()
# --- expect ---
# 600 800
# [1, 2]
# (7, 8)
# {9}
# b'q'
