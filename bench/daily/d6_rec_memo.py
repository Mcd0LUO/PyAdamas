"""d6_rec_memo -- 数值算子型：递归 + 记忆化。

fibonacci（lru_cache）、网格路径数（双参 lru_cache）、编辑距离
（手写 dict[str,int] 记忆化）、零钱兑换最少硬币（lru_cache）、
Catalan 数（记忆化递归）。全整数结果，确定性。
"""
import functools


@functools.lru_cache(maxsize=None)
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


@functools.lru_cache(maxsize=None)
def grid(m: int, n: int) -> int:
    if m == 0 or n == 0:
        return 1
    return grid(m - 1, n) + grid(m, n - 1)


def edit_distance(a: str, b: str) -> int:
    memo: dict[str, int] = {}

    def ed(i: int, j: int) -> int:
        if i == 0:
            return j
        if j == 0:
            return i
        key: str = str(i) + "," + str(j)
        if key in memo:
            return memo[key]
        cost: int = 0 if a[i - 1] == b[j - 1] else 1
        v: int = min(ed(i - 1, j) + 1, ed(i, j - 1) + 1, ed(i - 1, j - 1) + cost)
        memo[key] = v
        return v

    return ed(len(a), len(b))


@functools.lru_cache(maxsize=None)
def min_coins(amount: int) -> int:
    if amount == 0:
        return 0
    if amount < 0:
        return 1000000000
    best: int = 1000000000
    for c in [1, 5, 10, 25]:
        v: int = min_coins(amount - c) + 1
        if v < best:
            best = v
    return best


def catalan(n: int) -> int:
    memo: dict[int, int] = {0: 1}

    def cat(k: int) -> int:
        if k in memo:
            return memo[k]
        s: int = 0
        for i in range(k):
            s += cat(i) * cat(k - 1 - i)
        memo[k] = s
        return s

    return cat(n)


def main() -> None:
    print("-- fibonacci --")
    for n in [10, 30, 50, 80]:
        print("fib", n, fib(n))
    print("-- grid paths --")
    for mn in [(2, 2), (5, 5), (10, 10), (15, 15)]:
        print("grid", mn[0], mn[1], grid(mn[0], mn[1]))
    print("-- edit distance --")
    pairs: list[list[str]] = [["kitten", "sitting"], ["sunday", "saturday"],
                              ["algorithm", "altruistic"], ["", "abc"], ["same", "same"]]
    for p in pairs:
        print(p[0], p[1], edit_distance(p[0], p[1]))
    print("-- min coins --")
    for amt in [1, 6, 18, 47, 99, 289]:
        print("coins", amt, min_coins(amt))
    print("-- catalan --")
    for n in [1, 5, 10, 15, 20]:
        print("catalan", n, catalan(n))


if __name__ == "__main__":
    main()
