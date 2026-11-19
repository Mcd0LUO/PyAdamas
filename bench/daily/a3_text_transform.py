"""a3_text_transform -- 快速往返型：短文本变换 CLI。

对 argv 给出的单词按命令做变换（lower/upper/reverse/slug/title/rot13），
无参数时跑内置演示。纯字符串操作、即时计算、运行极快。
"""
import sys


def slugify(word: str) -> str:
    out: list[str] = []
    prev_dash: bool = False
    for ch in word.lower():
        if ch.isalnum():
            out.append(ch)
            prev_dash = False
        else:
            if not prev_dash and len(out) > 0:
                out.append("-")
                prev_dash = True
    s: str = "".join(out)
    return s.strip("-")


def rot13(word: str) -> str:
    out: list[str] = []
    for ch in word:
        if "a" <= ch <= "z":
            out.append(chr((ord(ch) - 97 + 13) % 26 + 97))
        elif "A" <= ch <= "Z":
            out.append(chr((ord(ch) - 65 + 13) % 26 + 65))
        else:
            out.append(ch)
    return "".join(out)


def transform(cmd: str, word: str) -> str:
    if cmd == "lower":
        return word.lower()
    if cmd == "upper":
        return word.upper()
    if cmd == "reverse":
        return word[::-1]
    if cmd == "slug":
        return slugify(word)
    if cmd == "title":
        return word.title()
    if cmd == "rot13":
        return rot13(word)
    if cmd == "swap":
        return word.upper() if word.islower() else word.lower()
    return word


def main() -> None:
    args: list[str] = sys.argv[1:]
    cmds: list[str] = ["lower", "upper", "reverse", "slug", "title", "rot13", "swap"]
    if len(args) == 0:
        words: list[str] = ["Hello", "World", "Foo-Bar", "baz_qux", "ABC", "mixedCase"]
        for w in words:
            row: list[str] = []
            for c in cmds:
                row.append(c + "=" + transform(c, w))
            print(w, "->", " ".join(row))
    else:
        cmd: str = args[0]
        for w in args[1:]:
            print(w, "->", transform(cmd, w))


if __name__ == "__main__":
    main()
