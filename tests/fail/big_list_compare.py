# expect: container comparisons involving big are not supported
n: big = 7
print([n] == [n + 0])
