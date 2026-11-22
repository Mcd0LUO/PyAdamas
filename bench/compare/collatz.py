# collatz —— 1..N 的 Collatz 步数总和与最长链（整数分支 / 除法）
N = 50000


def steps(n: int) -> int:
    c = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        c = c + 1
    return c


total = 0
best = 0
bestn = 1
i = 1
while i < N:
    s = steps(i)
    total = total + s
    if s > best:
        best = s
        bestn = i
    i = i + 1
print(total, best, bestn)
