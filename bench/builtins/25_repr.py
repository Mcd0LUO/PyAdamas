# 25_repr —— repr(整数)
N = 500000
total = 0
for i in range(N):
    total += len(repr(i))
print(total)
