# 未知种类元素的相等/包含语义（0.424：list_value_eq / chain_elem_eq 的 kind=3/"x"）
#
# 背景：静态类型推断不出来的元素（混合容器 list[unknown]）在运行期按「机器字」比指针：
#   ["a" + str(0), 1] 里 `("a" + str(0)) in xs` / `xs.index(...)` / `xs == ys` 都会错
#   （等值的新 str 对象查不到 / 不相等）。修法与 0.422 的 dict key_kind=3 同思路：
#   运行时认出堆 str → 按内容比；big → 按数值比；容器/类实例仍按同一性（避免把
#   list == tuple 误判成相等）。


def contains(xs, p) -> int:
    if p in xs:
        return 1
    return 0


def eq(a, b) -> bool:
    return a == b


# 1) 混合 list[unknown]：in / index / ==
xs = ["a" + str(0), 1, 2.5]
p = "a" + str(0)
print("mixed_in", contains(xs, p))
print("mixed_notin", contains(xs, "b" + str(9)))
print("mixed_index", xs.index(p))

ys = ["a" + str(0), 1]
zs = ["a" + str(0), 1]
print("mixed_eq", eq(ys, zs))
print("mixed_eq_self", eq(ys, ys))
ws = ["a" + str(0), 2]
print("mixed_neq", eq(ys, ws))

# 2) 已知类型（list[str]）不受影响：内容比较
strs = []
for i in range(4):
    strs.append("s" + str(i))
print("typed_in", contains(strs, "s" + str(2)))
print("typed_index", strs.index("s" + str(2)))

# 3) 未知元素里的 big（按数值比）
bigs = [10 ** 20, 1]
print("big_in", contains(bigs, 10 ** 20))

# 4) 跨类型：list == tuple 必须仍然 False（静态类型已知，走类型化比较路径）
lt = ["z" + str(0)]
tp = ("z" + str(0),)
print("list_vs_tuple", lt == tp)

# 5) 负例：int 元素、0/负数
nums = [0, -1, 5]
print("int_in", contains(nums, 0), contains(nums, -1), contains(nums, 7))

# 6) 已知类型嵌套容器（list[list[int]]）：比较走类型化 chain 路径
n1 = [[1, 2], [3]]
n2 = [[1, 2], [3]]
print("nested_known_eq", n1 == n2, n2 == [[1, 2], [3]])

# --- expect ---
# mixed_in 1
# mixed_notin 0
# mixed_index 0
# mixed_eq True
# mixed_eq_self True
# mixed_neq False
# typed_in 1
# typed_index 2
# big_in 1
# list_vs_tuple False
# int_in 1 1 0
# nested_known_eq True True
