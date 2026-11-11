# 22_oct —— oct(整数)
N = 500000
total = 0
for i in range(N):
    total += len(oct(i))
print(total)
