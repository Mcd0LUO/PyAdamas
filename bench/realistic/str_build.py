# str_build —— 字符串拼接与切片（字符串密集型）
N = 66000

def build_line(n: int) -> str:
    parts = []
    for i in range(n):
        parts.append(str(i % 10))
    return "".join(parts)

def transform(s: str) -> str:
    out = []
    for i in range(len(s)):
        c = s[i]
        if c == "0":
            out.append("_")
        else:
            out.append(c)
    return "".join(out)

total = 0
for r in range(N):
    line = build_line(60)
    t = transform(line)
    total += len(t)
print(total)
