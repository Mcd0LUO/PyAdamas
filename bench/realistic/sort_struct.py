# sort_struct —— 结构体排序与聚合（排序密集型）
N = 3900

def make_records(seed: int, n: int) -> list[tuple[int, int, int]]:
    out = []
    x = seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) % 2147483648
        a = x % 1000
        x = (x * 1103515245 + 12345) % 2147483648
        b = x % 1000
        out.append((a, b, a + b))
    return out

def aggregate(recs: list[tuple[int, int, int]]) -> int:
    s = 0
    for r in recs:
        s += r[2]
    return s

total = 0
for r in range(N):
    recs = make_records(r, 500)
    recs.sort()
    total += aggregate(recs)
print(total)
