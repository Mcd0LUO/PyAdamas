# 53_iter_cjk —— for ch in str（游标保证线性）
N = 3000
S = "中文测试字符串abc" * 64
total = 0
for i in range(N):
    for ch in S:
        total += 1
print(total)
