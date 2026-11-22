# 未知元素种类排序（kind=3）按 CPython 口径：族混杂抛 TypeError（0.425）
# 附带回归：已知类型排序/min-max、deque.index/count、类实例 __eq__（静态类型域）。

# 1) 混合容器 list[unknown]：不可比较的元素必须抛 TypeError，不能静默按机器字排
xs = ["a" + str(0), 1]
try:
    sorted(xs)
    print("mixed_sorted_no_error")
except TypeError:
    print("mixed_sorted_typeerror")
try:
    xs.sort()
    print("mixed_sort_inplace_no_error")
except TypeError:
    print("mixed_sort_inplace_typeerror")

# 2) 已知类型：内容排序/min/max 回归
ys = [3, 1, 2]
print("int_sorted", sorted(ys))
print("int_inplace", sorted(ys))
print("int_minmax", min(ys), max(ys))
ss = ["b", "a", "c"]
print("str_sorted", sorted(ss), min(ss), max(ss))
zs = [3.0, 1.5, 2.0]
print("num_sorted", sorted(zs))
bs = [True, False, True]
print("bool_sorted", sorted(bs))

# 3) deque：index/count 走内容比较（elem kind 已知）
from collections import deque
dq = deque(["q" + str(2), "q" + str(0), "q" + str(1)])
p = "q" + str(1)
print("deque_index", dq.index(p))
print("deque_count", dq.count(p))

# 4) 类实例 __eq__（静态类型域，typed 路径本来支持）
class P:
    def __init__(self, v: int):
        self.v = v

    def __eq__(self, other: P) -> bool:
        return self.v == other.v


p1 = P(1)
p2 = P(1)
print("class_eq", p1 == p2, p1 != P(2))

# --- expect ---
# mixed_sorted_typeerror
# mixed_sort_inplace_typeerror
# int_sorted [1, 2, 3]
# int_inplace [1, 2, 3]
# int_minmax 1 3
# str_sorted ['a', 'b', 'c'] a c
# num_sorted [1.5, 2.0, 3.0]
# bool_sorted [False, True, True]
# deque_index 2
# deque_count 1
# class_eq True True
