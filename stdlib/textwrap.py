"""textwrap 模块的 pyadamas 子集（纯 pyadamas 源码）。

覆盖 wrap / fill / dedent / indent / shorten 与 TextWrapper 类的常用行为；
接受 initial_indent / subsequent_indent / break_long_words / break_on_hyphens
参数但忽略后两个（超长单词不拆分）。
"""


def _wrap_impl(text: str, width: int, initial_indent: str,
               subsequent_indent: str) -> list[str]:
    if width <= 0:
        raise ValueError("width must be positive")
    words = text.split()
    lines: list[str] = []
    cur = ""
    prefix = initial_indent
    for w in words:
        if len(cur) == 0:
            cur = w
        elif len(prefix) + len(cur) + 1 + len(w) <= width:
            cur = cur + " " + w
        else:
            lines.append(prefix + cur)
            prefix = subsequent_indent
            cur = w
    if len(cur) > 0:
        lines.append(prefix + cur)
    return lines


def wrap(text: str, width: int = 70, initial_indent: str = "",
         subsequent_indent: str = "", break_long_words: bool = True,
         break_on_hyphens: bool = True) -> list[str]:
    """按空白切词，贪心装行；超长单词不拆分。"""
    return _wrap_impl(text, width, initial_indent, subsequent_indent)


def fill(text: str, width: int = 70, initial_indent: str = "",
         subsequent_indent: str = "", break_long_words: bool = True,
         break_on_hyphens: bool = True) -> str:
    lines = _wrap_impl(text, width, initial_indent, subsequent_indent)
    return "\n".join(lines)


class TextWrapper:
    """CPython TextWrapper 的常用子集：属性 + wrap()/fill()。

    语义边界：break_long_words / break_on_hyphens 只保存不生效；
    没有 max_lines / placeholder / expand_tabs / replace_whitespace。
    """

    width: int
    initial_indent: str
    subsequent_indent: str
    break_long_words: bool
    break_on_hyphens: bool

    def __init__(self, width: int = 70, initial_indent: str = "",
                 subsequent_indent: str = "",
                 break_long_words: bool = True,
                 break_on_hyphens: bool = True) -> None:
        self.width = width
        self.initial_indent = initial_indent
        self.subsequent_indent = subsequent_indent
        self.break_long_words = break_long_words
        self.break_on_hyphens = break_on_hyphens

    def wrap(self, text: str) -> list[str]:
        return _wrap_impl(text, self.width, self.initial_indent,
                          self.subsequent_indent)

    def fill(self, text: str) -> str:
        return "\n".join(self.wrap(text))


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


def shorten(text: str, width: int = 70, placeholder: str = "...") -> str:
    if width <= 0:
        return ""
    if len(text) <= width:
        return text
    if width <= len(placeholder):
        return placeholder[:width]
    words = text.split()
    out = ""
    for w in words:
        cand = w if len(out) == 0 else out + " " + w
        if len(cand) + len(placeholder) <= width:
            out = cand
        else:
            break
    return out + placeholder
