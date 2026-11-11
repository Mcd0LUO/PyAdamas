# 06_str —— str(整数) 转换（用 len 量结果长度）
N = 500000
total = 0
for i in range(N):
    total += len(str(i))
print(total)
