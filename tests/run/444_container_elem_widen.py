# 444: 容器元素 int→big 的加宽必须逐元素转换。
# 列表长度是动态的，不能像元组那样按常量下标重建 —— 降级层发拷贝循环：
# 新建 list[big]，逐元素 convert_to 后 append。不做的话元素仍是原始 int，
# 读取时被当 big 指针解引用（实测段错误）。


def ints() -> list[int]:
    return [5, 6]


xs: list[big] = ints()
print(sorted(xs))
print(5 in xs)
print(sum(xs))


def small_ints() -> list[int]:
    return [7, 8]


ys: list[big] = small_ints()
print(ys)

def mk_set() -> set[int]:
    return set([5, 6])


s: set[big] = mk_set()
print(len(s))
print(5 in s)
print(sorted(s))
# --- expect ---
# [5, 6]
# True
# 11
# [7, 8]
# 2
# True
# [5, 6]
