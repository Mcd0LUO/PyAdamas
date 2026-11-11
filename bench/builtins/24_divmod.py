# 24_divmod —— divmod(整数, 整数)
N = 1000000
total = 0
for i in range(N):
    q, r = divmod(i, 7)
    total += q + r
print(total)
