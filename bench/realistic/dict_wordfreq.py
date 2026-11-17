# dict_wordfreq —— 词频统计 + 排序（字典密集型）
N = 12000

WORDS = ["alpha","beta","gamma","delta","epsilon","zeta","eta","theta",
         "iota","kappa","lambda","mu","nu","xi","omicron","pi"]

def make_text(seed: int, n: int) -> list[str]:
    out = []
    x = seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) % 2147483648
        out.append(WORDS[x % 16])
    return out

def count_words(words: list[str]) -> dict[str, int]:
    counts = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    return counts

def top_n(counts: dict[str, int], n: int) -> list[str]:
    items = []
    for k in counts:
        items.append((counts[k], k))
    items.sort()
    items.reverse()
    res = []
    for i in range(n):
        res.append(items[i][1])
    return res

total = 0
for r in range(N):
    words = make_text(r, 400)
    counts = count_words(words)
    tops = top_n(counts, 3)
    total += len(tops)
print(total)
