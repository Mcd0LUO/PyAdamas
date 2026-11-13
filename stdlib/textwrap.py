"""textwrap 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 wrap / fill / dedent / indent 的常用行为；不建模
break_long_words / break_on_hyphens / initial_indent /
subsequent_indent / predicate / TextWrapper 类。
"""


def wrap(text: str, width: int = 70) -> list[str]:
    """按空白切词，贪心装行；超长单词不拆分。"""
    if width <= 0:
        raise ValueError("width must be positive")
    words = text.split()
    lines: list[str] = []
    cur = ""
    for w in words:
        if len(cur) == 0:
            cur = w
        elif len(cur) + 1 + len(w) <= width:
            cur = cur + " " + w
        else:
            lines.append(cur)
            cur = w
    if len(cur) > 0:
        lines.append(cur)
    return lines


def fill(text: str, width: int = 70) -> str:
    return "\n".join(wrap(text, width))


def dedent(text: str) -> str:
    """去掉所有非空行共有的前导空白。"""
    lines = text.split("\n")
    indent = -1
    for line in lines:
        stripped = line.lstrip()
        if len(stripped) == 0:
            continue
        n = len(line) - len(stripped)
        if indent < 0 or n < indent:
            indent = n
    if indent <= 0:
        return text
    out: list[str] = []
    for line in lines:
        if len(line) >= indent:
            out.append(line[indent:])
        else:
            out.append("")
    return "\n".join(out)


def indent(text: str, prefix: str) -> str:
    """给每个非空行加前缀。"""
    lines = text.split("\n")
    out: list[str] = []
    for line in lines:
        if len(line) > 0:
            out.append(prefix + line)
        else:
            out.append(line)
    return "\n".join(out)
