# 26_format —— format(整数, "d")
N = 500000
total = 0
for i in range(N):
    total += len(format(i, "d"))
print(total)
