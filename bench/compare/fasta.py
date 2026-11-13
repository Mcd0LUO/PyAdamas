# fasta —— 伪随机 DNA 序列生成 + 字符串拼接（Benchmarks Game 经典基准）
N = 40000


def build(n):
    seed = 42
    parts = []
    total = 0
    while total < n:
        seed = (seed * 1103515245 + 12345) % 2147483648
        r = seed % 4
        if r == 0:
            parts.append("ACGT")
        elif r == 1:
            parts.append("AAAA")
        elif r == 2:
            parts.append("CCCC")
        else:
            parts.append("GGGG")
        total += 4
    return "".join(parts)


s = build(N)
h = 0
i = 0
while i < N:
    h = (h * 31 + ord(s[i])) % 1000000007
    i += 1
print(N)
print(h)
