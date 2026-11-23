"""shlex —— 简单词法切分（posix 模式子集，纯 pyadamas 源码）。

实现 split(s) / quote(s) / join(parts)：
- split(s)：空白分隔；单引号内所有字符（含反斜杠）按字面；双引号内
  反斜杠只对双引号和反斜杠自身转义；引号外的反斜杠转义下一个字符；未闭合引号
  抛 ValueError("No closing quotation")；反斜杠结尾抛
  ValueError("No escaped character")。注释/分隔符 raw 字符串按普通字符
  处理（posix 模式注释符默认关闭）。
- quote(s)：CPython shlex.quote 的字节级规则（安全字符集
  [A-Za-z0-9_@%+=:,./-] 不引用；其余用单引号包住，内部单引号写成
  '"'"'）。
- join(parts)：" ".join(quote(p) for p in parts)；round-trip 满足
  split(join(parts)) == parts。

只支持 str 输入，没有 shlex 类 / posix=False / punctuation_chars。
"""


def _is_safe(c: str) -> bool:
    if c >= "a" and c <= "z":
        return True
    if c >= "A" and c <= "Z":
        return True
    if c >= "0" and c <= "9":
        return True
    return (c == "_" or c == "@" or c == "%" or c == "+" or c == "=" or
            c == ":" or c == "," or c == "." or c == "/" or c == "-")


def quote(s: str) -> str:
    """按 CPython shlex.quote 的规则给单个词加 shell 引号。"""
    if s == "":
        return "''"
    safe: bool = True
    i: int = 0
    while i < len(s):
        if not _is_safe(s[i]):
            safe = False
            break
        i += 1
    if safe:
        return s
    out: str = "'"
    i = 0
    while i < len(s):
        if s[i] == "'":
            # CPython 的写法是 '"'"，即 单引号-双引号-单引号-双引号-单引号。
            out = out + "'" + chr(34) + "'" + chr(34) + "'"
        else:
            out = out + s[i]
        i += 1
    return out + "'"


def join(parts: list[str]) -> str:
    """把词列表拼成可用 split 还原的 shell 命令行字符串。"""
    out: str = ""
    i: int = 0
    while i < len(parts):
        if i > 0:
            out = out + " "
        out = out + quote(parts[i])
        i += 1
    return out


def split(s: str) -> list[str]:
    """按 posix shell 词法切分字符串，返回词列表。"""
    words: list[str] = []
    buf: list[str] = []
    started: bool = False
    i: int = 0
    n: int = len(s)
    while i < n:
        c = s[i]
        if c == " " or c == "\t" or c == "\n" or c == "\r":
            if started:
                words.append("".join(buf))
                buf = []
                started = False
            i += 1
            continue
        if c == "'":
            started = True
            i += 1
            closed: bool = False
            while i < n:
                if s[i] == "'":
                    closed = True
                    i += 1
                    break
                buf.append(s[i])
                i += 1
            if not closed:
                raise ValueError("No closing quotation")
            continue
        if c == '"':
            started = True
            i += 1
            closed = False
            while i < n:
                ch = s[i]
                if ch == '"':
                    closed = True
                    i += 1
                    break
                if ch == "\\":
                    if i + 1 < n and (s[i + 1] == '"' or s[i + 1] == "\\"):
                        buf.append(s[i + 1])
                        i += 2
                        continue
                    buf.append("\\")
                    i += 1
                    continue
                buf.append(ch)
                i += 1
            if not closed:
                raise ValueError("No closing quotation")
            continue
        if c == "\\":
            started = True
            if i + 1 >= n:
                raise ValueError("No escaped character")
            buf.append(s[i + 1])
            i += 2
            continue
        buf.append(c)
        started = True
        i += 1
    if started:
        words.append("".join(buf))
    return words
