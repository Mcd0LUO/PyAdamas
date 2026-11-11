# 21_bin —— bin(整数)
N = 500000
total = 0
for i in range(N):
    total += len(bin(i))
print(total)
