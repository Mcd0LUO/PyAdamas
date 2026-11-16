# 52_index_cjk —— 非 ASCII 串的顺序下标（有游标时应线性）
N = 100000
S = "中文测试字符串abc" * 64
n = len(S)
total = 0
for i in range(N):
    total += len(S[i % n])
print(total)
