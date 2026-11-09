print(abs(-5))
print(abs(-2.5))
print(min(3, 1, 2))
print(max(3, 1, 2))
print(min(2.5, 1.5))
print(max(2, 3.5))
print(min([5, 2, 9]))
print(max((5, 2, 9)))
print(min("banana"))
print(max("banana"))
print(sum([1, 2, 3]))
print(sum((1.5, 2.5)))
print(sum([], 10))
print(sum([1, 2], 10))
print(min({"b": 1, "a": 2}))
print(max({3, 1, 2}))
print(pow(2, 10))
print(pow(2.5, 2))
try:
    min([])
except ValueError as e:
    print("empty", e)
try:
    max(())
except ValueError as e:
    print("empty2", e)
print(any([0, 1, 2]))
print(any([]))
print(all([1, 2, 3]))
print(all([1, 0]))
print(all([]))
print(any(["", "a"]))
print(all(["a", "b"]))
print(any({"", "x"}))
print(all([True, False]))
print(any(["", ""]))

# --- expect ---
# 5
# 2.5
# 1
# 3
# 1.5
# 3.5
# 2
# 9
# a
# n
# 6
# 4.0
# 10
# 13
# a
# 3
# 1024
# 6.25
# empty min() arg is an empty sequence
# empty2 max() arg is an empty sequence
# True
# False
# True
# False
# True
# True
# True
# True
# False
# False
