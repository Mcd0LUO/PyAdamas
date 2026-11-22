# sieve —— 埃氏筛计数（list[int] 逐元素写 + 跨步内层循环）
N = 500000

flags: list[int] = []
i = 0
while i < N:
    flags.append(1)
    i = i + 1
flags[0] = 0
flags[1] = 0

p = 2
while p * p < N:
    if flags[p] == 1:
        q = p * p
        while q < N:
            flags[q] = 0
            q = q + p
    p = p + 1

count = 0
i = 0
while i < N:
    if flags[i] == 1:
        count = count + 1
    i = i + 1
print(count)
