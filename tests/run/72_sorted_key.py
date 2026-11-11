xs = [3, 1, 2]
print(sorted(xs))
print(sorted(xs, key=lambda x: -x))
print(sorted(xs, reverse=True))
print(sorted(xs, key=lambda x: x, reverse=True))
words = ["bb", "a", "ccc"]
print(sorted(words, key=len))
print(xs)
pairs = [(2, "b"), (1, "a")]
print(sorted(pairs, key=lambda p: p[0]))
# --- expect ---
# [1, 2, 3]
# [3, 2, 1]
# [3, 2, 1]
# [3, 2, 1]
# ['a', 'bb', 'ccc']
# [3, 1, 2]
# [(1, 'a'), (2, 'b')]
