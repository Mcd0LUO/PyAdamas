# 19_chr —— chr(码点)（用 ord 取回长度）
N = 500000
total = 0
for i in range(N):
    total += ord(chr(33 + i % 90))
print(total)
