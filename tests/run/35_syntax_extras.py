d = {"a": 1, "b": 2}
del d["a"]
print(len(d), d["b"])

l = [1, 2, 3, 4]
del l[1]
print(l[0], l[1], len(l))
del l[0:2]
print(l[0], len(l))

args = [1, 2, 3]
print(*args)
print("x", *args, "y")
print(*args, sep="|", end=".\n")
print(*[], "end")
print(*["a", "b"], sep="-")
print(*{"k": 1, "z": 2})
print(*"hi", sep="/")

c = 3j
print(c)
print(1 + 2j)
print((1 + 2j) * (3 - 4j))
print((1 + 2j) / (1 - 1j))
print(abs(3 + 4j))
print(complex(1, 2))
print(complex())
print(1j == 1j, 1j == 2j)
print(-(2 + 3j))
print(f"{2 + 3j}")
print(bool(0j), bool(2j))

# --- expect ---
# 1 2
# 1 3 3
# 4 1
# 1 2 3
# x 1 2 3 y
# 1|2|3.
# end
# a-b
# k z
# h/i
# 3j
# (1+2j)
# (11+2j)
# (-0.5+1.5j)
# 5.0
# (1+2j)
# 0j
# True False
# (-2-3j)
# (2+3j)
# False True
