"""string 模块的 py23 子集（纯 py23 源码，随编译器分发）。"""

ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = digits + "abcdef" + "ABCDEF"
octdigits = "01234567"
punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace = " \t\n\r\v\f"
printable = digits + ascii_letters + punctuation + whitespace


def capwords(s: str) -> str:
    """按空白切词，每个词首字母大写后用一个空格连接。"""
    words: list[str] = s.split()
    out: list[str] = []
    i = 0
    while i < len(words):
        out.append(words[i].capitalize())
        i += 1
    return " ".join(out)
