def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""b4_wordfreq -- 字符串处理型：词频统计。

确定性生成一段约 3 万词的文本，分词/小写/去标点/停用词过滤，
用 Counter 计数，most_common + 字母序 tie-break 输出 Top-N 与分布统计。
"""
import re
from collections import Counter

WORD_RE = re.compile(r"[a-z]+")
STOP: set[str] = {"the", "a", "an", "and", "or", "of", "to", "in", "is", "it", "for", "on", "with", "as", "at", "by"}


def gen_text(nwords: int) -> str:
    vocab: list[str] = ["apple", "banana", "cherry", "date", "elder", "fig", "grape", "honey",
                        "kiwi", "lemon", "mango", "nut", "orange", "pear", "quince", "raisin",
                        "the", "and", "of", "to", "a", "in", "is", "it", "for", "on", "with"]
    parts: list[str] = []
    state: int = 246813579
    for i in range(nwords):
        state = lcg(state)
        w: str = vocab[state % len(vocab)]
        state = lcg(state)
        if state % 7 == 0:
            w = w.upper()
        if state % 11 == 0:
            w = w + ","
        elif state % 13 == 0:
            w = w + "."
        parts.append(w)
        if i % 12 == 11:
            parts.append("\n")
    return " ".join(parts)


def main() -> None:
    text: str = gen_text(30000)
    lowered: str = text.lower()
    tokens: list[str] = WORD_RE.findall(lowered)
    filtered: list[str] = []
    for t in tokens:
        if t not in STOP and len(t) > 1:
            filtered.append(t)
    counts: dict[str, int] = Counter(filtered)
    print("raw_tokens", len(tokens), "kept", len(filtered), "unique", len(counts))
    total_kept: int = 0
    for w in counts.keys():
        total_kept += counts[w]
    print("total_kept", total_kept)
    print("-- top 15 (count desc, alpha tie-break) --")
    alpha: list[str] = sorted(counts.keys())

    def neg_count(w: str) -> int:
        return -counts[w]

    ranked: list[str] = sorted(alpha, key=neg_count)
    for w in ranked[:15]:
        print(w, counts[w])
    print("-- most_common(5) --")
    mc: list[tuple[str, int]] = counts.most_common(5)
    for pair in mc:
        print(pair[0], pair[1])
    once: int = 0
    heavy: int = 0
    for w in counts.keys():
        if counts[w] == 1:
            once += 1
        if counts[w] > 1000:
            heavy += 1
    print("hapax", once, "heavy_gt1000", heavy)
    longest: str = ""
    for w in counts.keys():
        if len(w) > len(longest):
            longest = w
    print("longest_word", longest)


if __name__ == "__main__":
    main()
