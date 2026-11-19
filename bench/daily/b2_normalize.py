"""b2_normalize -- 字符串处理型：文本规范化 + slugify 流水线。

确定性生成含杂乱空白/标点/大小写的短语，做规范化（折叠空白、
去标点、ASCII 小写、标题化），再批量 slugify 并去重加后缀。
大量 split/join/replace/strip 与逐字符扫描。
"""
import re

WS_RE = re.compile(r"\s+")
PUNCT_RE = re.compile(r"[^a-z0-9]+")


def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


def normalize(text: str) -> str:
    t: str = WS_RE.sub(" ", text).strip()
    t = t.lower()
    words: list[str] = t.split(" ")
    out: list[str] = []
    for w in words:
        if len(w) == 0:
            continue
        cleaned: str = ""
        for ch in w:
            if ch.isalnum():
                cleaned += ch
        if len(cleaned) > 0:
            out.append(cleaned)
    return " ".join(out)


def titleize(text: str) -> str:
    parts: list[str] = text.split(" ")
    out: list[str] = []
    for p in parts:
        if len(p) > 0:
            out.append(p.capitalize())
    return " ".join(out)


def slugify(text: str) -> str:
    s: str = PUNCT_RE.sub("-", text.lower())
    return s.strip("-")


def gen_phrases(n: int) -> list[str]:
    words: list[str] = ["Hello", "World", "Foo", "Bar", "Baz", "Quick", "Brown", "Fox",
                        "Lazy", "Dog", "Data", "Pipeline", "Report", "2026", "Q4", "Summary"]
    seps: list[str] = ["  ", "\t", " ", ", ", " - ", "...", "   "]
    phrases: list[str] = []
    state: int = 987654321
    for i in range(n):
        k: int = 3 + (i % 4)
        parts: list[str] = []
        for j in range(k):
            state = lcg(state)
            w: str = words[state % len(words)]
            state = lcg(state)
            if state % 3 == 0:
                w = w.upper()
            elif state % 3 == 1:
                w = w.lower()
            parts.append(w)
        state = lcg(state)
        sep: str = seps[state % len(seps)]
        phrase: str = sep.join(parts)
        if i % 5 == 0:
            phrase = "  " + phrase + "!! "
        phrases.append(phrase)
    return phrases


def main() -> None:
    phrases: list[str] = gen_phrases(2000)
    slugs: list[str] = []
    seen: dict[str, int] = {}
    norm_len: int = 0
    for ph in phrases:
        norm: str = normalize(ph)
        norm_len += len(norm)
        slug: str = slugify(norm)
        if slug in seen:
            seen[slug] += 1
            slug = slug + "-" + str(seen[slug])
        else:
            seen[slug] = 1
        slugs.append(slug)
    print("phrases", len(phrases), "norm_total_len", norm_len)
    print("unique_slugs", len(seen))
    print("-- sample normalized --")
    for i in range(6):
        print(normalize(phrases[i]), "=>", slugs[i])
    print("-- titleized sample --")
    for i in range(4):
        print(titleize(normalize(phrases[i])))
    dupes: int = 0
    for s in seen.keys():
        if seen[s] > 1:
            dupes += 1
    print("slugs_with_dupes", dupes)
    longest: str = ""
    for s in slugs:
        if len(s) > len(longest):
            longest = s
    print("longest_slug", longest, len(longest))


if __name__ == "__main__":
    main()
