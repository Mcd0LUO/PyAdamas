# itertools 综合：组合生成器与扩展
# 0.381 合并自 2 个文件：142_itertools_combos.py, 183_itertools_ext.py

# ── 原 142_itertools_combos.py ──────────────────────────────────────

import itertools
from itertools import combinations

print(list(itertools.product([1, 2], [3, 4])))
print(list(itertools.product([1, 2], [3, 4], [5])))
print(list(itertools.permutations([1, 2, 3])))
print(list(itertools.permutations([1, 2, 3], 2)))
print(list(combinations([1, 2, 3, 4], 2)))
print(list(itertools.combinations_with_replacement([1, 2], 2)))
pairs = list(itertools.zip_longest([1, 2, 3], ["a", "b"]))
print(len(pairs))
print(pairs[0][0], pairs[0][1])
print(pairs[2][0], pairs[2][1] is None)
print(list(itertools.accumulate([1, 2, 3, 4])))
print(list(itertools.accumulate([1.0, 2.5, 3.0])))
print(list(itertools.product([], [1])))
print(list(itertools.combinations([1, 2], 3)))
print(list(itertools.product()))

# ── 原 183_itertools_ext.py ──────────────────────────────────────

# itertools 的 stdlib 扩展层（stdlib/itertools.py）：
# takewhile / dropwhile / filterfalse / compress / pairwise / groupby。
#
# 语义边界：本项目的 itertools 扩展统一接收 list、返回 list（不是惰性迭代器），
# 因为 PyAdamas 没有通用迭代协议 —— 形参不能标注成「任意可迭代」。
# 结果集合与 CPython 一致，只是求值时机不同。
import itertools

print(itertools.takewhile(lambda x: x < 3, [1, 2, 3, 4]))
print(itertools.dropwhile(lambda x: x < 3, [1, 2, 3, 4]))
print(itertools.filterfalse(lambda x: x % 2 == 0, [1, 2, 3, 4]))
print(itertools.compress([1, 2, 3, 4], [1, 0, 1, 0]))

# pairwise / groupby 返回 tuple 的列表：用下标取，避免依赖 tuple 的 print 表示。
pw = itertools.pairwise([1, 2, 3])
i: int = 0
while i < len(pw):
    print(pw[i][0])
    print(pw[i][1])
    i += 1

gb = itertools.groupby([1, 1, 2, 2, 2, 3])
j: int = 0
while j < len(gb):
    print(gb[j][0])
    print(len(gb[j][1]))
    j += 1

# --- expect ---
# [(1, 3), (1, 4), (2, 3), (2, 4)]
# [(1, 3, 5), (1, 4, 5), (2, 3, 5), (2, 4, 5)]
# [(1, 2, 3), (1, 3, 2), (2, 1, 3), (2, 3, 1), (3, 1, 2), (3, 2, 1)]
# [(1, 2), (1, 3), (2, 1), (2, 3), (3, 1), (3, 2)]
# [(1, 2), (1, 3), (1, 4), (2, 3), (2, 4), (3, 4)]
# [(1, 1), (1, 2), (2, 2)]
# 3
# 1 a
# 3 True
# [1, 3, 6, 10]
# [1.0, 3.5, 6.5]
# []
# []
# [()]
# [1, 2]
# [3, 4]
# [1, 3]
# [1, 3]
# 1
# 2
# 2
# 3
# 1
# 2
# 2
# 3
# 3
# 1
