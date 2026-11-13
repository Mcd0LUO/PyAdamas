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
