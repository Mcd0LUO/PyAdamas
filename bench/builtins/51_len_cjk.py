# 51_len_cjk —— len() 对非 ASCII 串（码点语义，应仍是 O(1)）
N = 200000
S = ["中文测试字符串", "abc", "中", "😀😀", "a中b文c", "纯中文内容测试", "x", "文" * 8]
total = 0
for i in range(N):
    total += len(S[i % 8])
print(total)
