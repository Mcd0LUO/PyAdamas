print(list(map(str, [1, 2, 3])))
print(list(filter(lambda x: x > 1, [1, 2, 3])))
print(sorted([3, 1, 2]))
print(sorted([3, 1, 2], reverse=True))
print(any([0, 1]))
print(all([1, 1]))
print(divmod(7, 2))
print(pow(2, 10))
print(round(3.567, 2))
print(repr("a"))
print(format(3.14, ".1f"))
print(format(42, "05d"))
print(min([3, 1, 2]), max([3, 1, 2]))
print(sum([1, 2, 3]))
print(abs(-5))
print(list(zip([1, 2], ["a", "b"])))
print(list(enumerate(["a", "b"])))
print(hex(255), oct(8), bin(5))
print(chr(65), ord("A"))
print(isinstance(1, int), callable(print))
print(hash("x") == hash("x"))

# --- expect ---
# ['1', '2', '3']
# [2, 3]
# [1, 2, 3]
# [3, 2, 1]
# True
# True
# (3, 1)
# 1024
# 3.57
# 'a'
# 3.1
# 00042
# 1 3
# 6
# 5
# [(1, 'a'), (2, 'b')]
# [(0, 'a'), (1, 'b')]
# 0xff 0o10 0b101
# A 65
# True True
# True
