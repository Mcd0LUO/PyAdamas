# rec_memo —— 递归 + 记忆化（递归/字典密集型）
N = 195000

def fib_memo(n: int, memo: dict[int, int]) -> int:
    if n < 2:
        return n
    if n in memo:
        return memo[n]
    v = fib_memo(n - 1, memo) + fib_memo(n - 2, memo)
    memo[n] = v
    return v

total = 0
for r in range(N):
    memo = {}
    total += fib_memo(25, memo)
print(total)
