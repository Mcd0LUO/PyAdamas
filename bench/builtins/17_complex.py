# 17_complex —— complex(i, 1) + abs()
N = 500000
total = 0.0
for i in range(N):
    total += abs(complex(i, 1))
print(total)
