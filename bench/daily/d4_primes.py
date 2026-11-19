"""d4_primes -- 数值算子型：素数筛与数论统计。

埃拉托斯特尼筛法求 20 万以内素数，统计素数个数/和、孪生素数、
素数间隙分布、Goldbach 抽查（偶数分解为两素数之和）。纯整数运算。
"""


def sieve(limit: int) -> list[bool]:
    mark: list[bool] = []
    for i in range(limit + 1):
        mark.append(True)
    mark[0] = False
    mark[1] = False
    i: int = 2
    while i * i <= limit:
        if mark[i]:
            j: int = i * i
            while j <= limit:
                mark[j] = False
                j += i
        i += 1
    return mark


def main() -> None:
    limit: int = 200000
    mark: list[bool] = sieve(limit)
    primes: list[int] = []
    for i in range(2, limit + 1):
        if mark[i]:
            primes.append(i)
    count: int = len(primes)
    psum: int = 0
    for p in primes:
        psum += p
    print("limit", limit, "prime_count", count, "prime_sum", psum)
    print("largest_prime", primes[count - 1])
    twins: int = 0
    for i in range(count - 1):
        if primes[i + 1] - primes[i] == 2:
            twins += 1
    print("twin_prime_pairs", twins)
    gaps: dict[int, int] = {}
    max_gap: int = 0
    for i in range(count - 1):
        g: int = primes[i + 1] - primes[i]
        gaps[g] = gaps.get(g, 0) + 1
        if g > max_gap:
            max_gap = g
    print("max_gap", max_gap)
    print("-- gap histogram (top) --")
    for g in sorted(gaps.keys())[:10]:
        print(g, gaps[g])
    print("-- Goldbach spot checks --")
    def is_prime(x: int) -> bool:
        if x < 2:
            return False
        return mark[x]
    for n in [100, 1000, 10000, 100000, 199998]:
        found: bool = False
        a: int = 2
        while a <= n // 2:
            if is_prime(a) and is_prime(n - a):
                print(n, "=", a, "+", n - a)
                found = True
                break
            a += 1
        if not found:
            print(n, "no decomposition")
    print("-- prime density by decade --")
    for lo in [0, 50000, 100000, 150000]:
        hi: int = lo + 50000
        c: int = 0
        for p in primes:
            if p >= lo and p < hi:
                c += 1
        print(lo, hi, c)


if __name__ == "__main__":
    main()
