ints = [5, -2, 10, 3, 7, 11, -8, 4]
print(sum(ints), min(ints), max(ints))
print(sum(ints, 100))

small = [9, 4, 7]
print(sum(small), min(small), max(small))

floats: list[float] = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5]
print(sum(floats), sum(floats, 1.5))

print(sum([], 7))
print(sum([], 0.5))

try:
    print(min([]))
except ValueError as e:
    print("min", str(e))
try:
    print(max([]))
except ValueError as e:
    print("max", str(e))

big: list[int] = []
for i in range(1000):
    big.append(i)
print(sum(big), min(big), max(big))

# --- expect ---
# 30 -8 11
# 130
# 20 4 9
# 24.0 25.5
# 7
# 0.5
# min min() iterable argument is empty
# max max() iterable argument is empty
# 499500 0 999
