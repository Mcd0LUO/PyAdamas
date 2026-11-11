# 20_hex —— hex(整数)
N = 500000
total = 0
for i in range(N):
    total += len(hex(i))
print(total)
