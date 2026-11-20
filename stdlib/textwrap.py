"""textwrap —— 段落折行与缩进（纯 pyadamas 源码，与 CPython textwrap 同口径）。

覆盖 CPython 3.14 textwrap 的公开面：TextWrapper 类（wrap/fill 及
_munge_whitespace/_split/_wrap_chunks/_handle_long_word/_fix_sentence_endings
子类钩子）、wrap / fill / shorten / dedent / indent。

参数面与 CPython 一致：width / initial_indent / subsequent_indent /
expand_tabs / tabsize / replace_whitespace / fix_sentence_endings /
break_long_words / drop_whitespace / break_on_hyphens / max_lines /
placeholder。错误口径一致：width <= 0 → ValueError("invalid width ...")，
max_lines 下 placeholder 放不下 → ValueError("placeholder too large for
max width")。

已知边界（纯 pyadamas 表示所限，均有负面说明）：
- 词字符按 ASCII 判（[A-Za-z0-9_]），CPython 的 \\w 对 str 是 Unicode 语义。
  纯 ASCII 文本逐字节一致；非 ASCII 词/连字符的断点可能与 CPython 不同。
- max_lines/深度这类「None 表示不限」的参数用真 None 默认值；实参显式传
  None 会被静态类型检查拒绝（CPython 允许的 W(None) 写法在 pa 侧编译报错）。
- indent 的换行集只覆盖 ASCII：\\n \\r \\r\\n \\v \\f \\x1c-\\x1e，
  不含 U+0085/U+2028/U+2029（pa 字符串没有这些码点的行分隔语义）。
"""

# CPython 的 _whitespace（US-ASCII 空白，故意不含 Unicode 空格）。
_WHITESPACE = " \t\n\x0b\x0c\r"


def _is_space(c: str) -> bool:
    return (c == " " or c == "\t" or c == "\n" or c == "\x0b"
            or c == "\x0c" or c == "\r")


def _is_letter(c: str) -> bool:
    """CPython wordsep_re 的 letter（[^\\d\\W] 的 ASCII 等价类）。"""
    return (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or c == "_"


def _is_word(c: str) -> bool:
    """ASCII 词字符 [A-Za-z0-9_]。"""
    return _is_letter(c) or (c >= "0" and c <= "9")


def _is_wp(c: str) -> bool:
    """CPython wordsep_re 的 word_punct：词字符 + ! " ' & . , ?"""
    if _is_word(c):
        return True
    return (c == "!" or c == '"' or c == "'" or c == "&" or c == "."
            or c == "," or c == "?")


def _emdash_at(text: str, i: int) -> bool:
    """text[i] 起是否是「词间 em-dash」：-{2,} 且后面跟词字符。"""
    n: int = len(text)
    if i >= n or text[i] != "-":
        return False
    j: int = i
    while j < n and text[j] == "-":
        j = j + 1
    if j - i < 2 or j >= n:
        return False
    return _is_word(text[j])


def _hyph_behind(text: str, j: int) -> bool:
    """连字符（下标 j）前是否符合 (?<=lt{2}-) | (?<=lt-lt-)。"""
    if j >= 2 and _is_letter(text[j - 2]) and _is_letter(text[j - 1]):
        return True
    if (j >= 3 and _is_letter(text[j - 3]) and text[j - 2] == "-"
            and _is_letter(text[j - 1])):
        return True
    return False


def _hyph_ahead(text: str, k: int) -> bool:
    """连字符后是否符合 (?=lt -? lt)，k 指向连字符后的第一个字符。"""
    n: int = len(text)
    if k >= n or not _is_letter(text[k]):
        return False
    if k + 1 >= n:
        return False
    if _is_letter(text[k + 1]):
        return True
    if text[k + 1] == "-" and k + 2 < n and _is_letter(text[k + 2]):
        return True
    return False


def _split_hyph(text: str) -> list[str]:
    """break_on_hyphens=True 的切块：CPython 3.14 wordsep_re 的等价扫描。

    chunk 要么是空白段，要么是「词」段；连字符断点把连字符留在前一段末尾，
    em-dash（词间 -- 以上）自成一段。
    """
    chunks: list[str] = []
    n: int = len(text)
    i: int = 0
    while i < n:
        c: str = text[i]
        if _is_space(c):
            j: int = i + 1
            while j < n and _is_space(text[j]):
                j = j + 1
            chunks.append(text[i:j])
            i = j
            continue
        # 空白 / em-dash / 词 三个分支按 CPython 的次序尝试。
        if c == "-" and i > 0 and _is_wp(text[i - 1]) and _emdash_at(text, i):
            j = i
            while j < n and text[j] == "-":
                j = j + 1
            chunks.append(text[i:j])
            i = j
            continue
        j = i + 1
        end: int = n
        while j < n:
            ch: str = text[j]
            if ch == "-" and _hyph_behind(text, j) and _hyph_ahead(text, j + 1):
                end = j + 1
                break
            if _is_space(ch):
                end = j
                break
            if ch == "-" and _is_wp(text[j - 1]) and _emdash_at(text, j):
                end = j
                break
            j = j + 1
        chunks.append(text[i:end])
        i = end
    return chunks


def _split_simple(text: str) -> list[str]:
    """break_on_hyphens=False 的切块：只按空白段切。"""
    chunks: list[str] = []
    n: int = len(text)
    i: int = 0
    while i < n:
        j: int = i + 1
        if _is_space(text[i]):
            while j < n and _is_space(text[j]):
                j = j + 1
        else:
            while j < n and not _is_space(text[j]):
                j = j + 1
        chunks.append(text[i:j])
        i = j
    return chunks


def _expand_tabs(text: str, tabsize: int) -> str:
    """str.expandtabs 的 ASCII 版：tab 补到下一个 tabsize 制表位。

    tabsize <= 0 时删掉 tab（与 CPython 一致）；\\n / \\r 把列号归零，
    其它字符（含 \\v / \\f）各占一列。
    """
    out: str = ""
    col: int = 0
    n: int = len(text)
    i: int = 0
    while i < n:
        c: str = text[i]
        if c == "\t":
            if tabsize > 0:
                k: int = tabsize - (col % tabsize)
                j: int = 0
                while j < k:
                    out = out + " "
                    j = j + 1
                col = col + k
        elif c == "\n" or c == "\r":
            out = out + c
            col = 0
        else:
            out = out + c
            col = col + 1
        i = i + 1
    return out


def _replace_ws(text: str) -> str:
    """把 \\t \\n \\v \\f \\r 逐字符换成空格（CPython unicode_whitespace_trans）。"""
    out: str = ""
    n: int = len(text)
    i: int = 0
    while i < n:
        c: str = text[i]
        if (c == "\t" or c == "\n" or c == "\x0b" or c == "\x0c"
                or c == "\r"):
            out = out + " "
        else:
            out = out + c
        i = i + 1
    return out


def _total_len(items: list[str]) -> int:
    total: int = 0
    for s in items:
        total = total + len(s)
    return total


def _has_non_hyphen(s: str) -> bool:
    for c in s:
        if c != "-":
            return True
    return False


def _sentence_end(chunk: str) -> bool:
    """CPython sentence_end_re 的等价判定：[a-z][.!?][\"']? 且贴住块尾。"""
    n: int = len(chunk)
    if n >= 1 and (chunk[n - 1] == '"' or chunk[n - 1] == "'"):
        n = n - 1
    if n < 2:
        return False
    c: str = chunk[n - 1]
    if c != "." and c != "!" and c != "?":
        return False
    p: str = chunk[n - 2]
    return p >= "a" and p <= "z"


def _splitlines_keepends(text: str) -> list[str]:
    """str.splitlines(True) 的 ASCII 子集（见模块头的边界说明）。"""
    lines: list[str] = []
    n: int = len(text)
    i: int = 0
    start: int = 0
    while i < n:
        c: str = text[i]
        if c == "\r":
            if i + 1 < n and text[i + 1] == "\n":
                i = i + 1
            lines.append(text[start:i + 1])
            i = i + 1
            start = i
        elif (c == "\n" or c == "\x0b" or c == "\x0c" or c == "\x1c"
              or c == "\x1d" or c == "\x1e"):
            lines.append(text[start:i + 1])
            i = i + 1
            start = i
        else:
            i = i + 1
    if start < n:
        lines.append(text[start:])
    return lines


class TextWrapper:
    """CPython TextWrapper 的 pyadamas 版（参数与语义同口径）。"""

    width: int
    initial_indent: str
    subsequent_indent: str
    expand_tabs: bool
    replace_whitespace: bool
    fix_sentence_endings: bool
    break_long_words: bool
    drop_whitespace: bool
    break_on_hyphens: bool
    tabsize: int
    max_lines: int
    placeholder: str

    def __init__(self, width: int = 70, initial_indent: str = "",
                 subsequent_indent: str = "", expand_tabs: bool = True,
                 replace_whitespace: bool = True,
                 fix_sentence_endings: bool = False,
                 break_long_words: bool = True, drop_whitespace: bool = True,
                 break_on_hyphens: bool = True, tabsize: int = 8,
                 max_lines: int = None, placeholder: str = " [...]") -> None:
        self.width = width
        self.initial_indent = initial_indent
        self.subsequent_indent = subsequent_indent
        self.expand_tabs = expand_tabs
        self.replace_whitespace = replace_whitespace
        self.fix_sentence_endings = fix_sentence_endings
        self.break_long_words = break_long_words
        self.drop_whitespace = drop_whitespace
        self.break_on_hyphens = break_on_hyphens
        self.tabsize = tabsize
        self.max_lines = max_lines
        self.placeholder = placeholder

    def _munge_whitespace(self, text: str) -> str:
        """展开 tab，再把其余 ASCII 空白换成空格。"""
        if self.expand_tabs:
            text = _expand_tabs(text, self.tabsize)
        if self.replace_whitespace:
            text = _replace_ws(text)
        return text

    def _split(self, text: str) -> list[str]:
        if self.break_on_hyphens:
            return _split_hyph(text)
        return _split_simple(text)

    def _fix_sentence_endings(self, chunks: list[str]) -> None:
        i: int = 0
        while i < len(chunks) - 1:
            if chunks[i + 1] == " " and _sentence_end(chunks[i]):
                chunks[i + 1] = "  "
                i = i + 2
            else:
                i = i + 1

    def _handle_long_word(self, rev: list[str], cur_line: list[str],
                          cur_len: int, width: int) -> None:
        if width < 1:
            space_left: int = 1
        else:
            space_left = width - cur_len
        if self.break_long_words and space_left > 0:
            end: int = space_left
            chunk: str = rev[len(rev) - 1]
            if self.break_on_hyphens and len(chunk) > space_left:
                hyphen: int = chunk.rfind("-", 0, space_left)
                if hyphen > 0 and _has_non_hyphen(chunk[:hyphen]):
                    end = hyphen + 1
            cur_line.append(chunk[:end])
            rev[len(rev) - 1] = chunk[end:]
        elif len(cur_line) == 0:
            cur_line.append(rev.pop())

    def _wrap_chunks(self, chunks: list[str]) -> list[str]:
        lines: list[str] = []
        if self.width <= 0:
            raise ValueError("invalid width " + repr(self.width)
                             + " (must be > 0)")
        if self.max_lines is not None:
            if self.max_lines > 1:
                indent: str = self.subsequent_indent
            else:
                indent = self.initial_indent
            if len(indent) + len(self.placeholder.lstrip()) > self.width:
                raise ValueError("placeholder too large for max width")
        rev: list[str] = []
        i: int = len(chunks) - 1
        while i >= 0:
            rev.append(chunks[i])
            i = i - 1
        while len(rev) > 0:
            remaining: int = _total_len(rev)
            cur_line: list[str] = []
            cur_len: int = 0
            if len(lines) > 0:
                indent = self.subsequent_indent
            else:
                indent = self.initial_indent
            width: int = self.width - len(indent)
            if (self.drop_whitespace and rev[len(rev) - 1].strip() == ""
                    and len(lines) > 0):
                rev.pop()
            while len(rev) > 0:
                l: int = len(rev[len(rev) - 1])
                if cur_len + l <= width:
                    cur_line.append(rev.pop())
                    cur_len = cur_len + l
                else:
                    break
            if len(rev) > 0 and len(rev[len(rev) - 1]) > width:
                self._handle_long_word(rev, cur_line, cur_len, width)
                cur_len = _total_len(cur_line)
            if (self.drop_whitespace and len(cur_line) > 0
                    and cur_line[len(cur_line) - 1].strip() == ""):
                cur_len = cur_len - len(cur_line[len(cur_line) - 1])
                cur_line.pop()
            if len(cur_line) > 0:
                if (self.max_lines is None
                        or len(lines) + 1 < self.max_lines
                        or ((len(rev) == 0
                             or (self.drop_whitespace and len(rev) == 1
                                 and rev[0].strip() == ""))
                            and cur_len <= width)):
                    lines.append(indent + "".join(cur_line))
                else:
                    placed: bool = False
                    while len(cur_line) > 0:
                        if (cur_line[len(cur_line) - 1].strip() != ""
                                and cur_len + len(self.placeholder) <= width):
                            cur_line.append(self.placeholder)
                            lines.append(indent + "".join(cur_line))
                            placed = True
                            break
                        cur_len = cur_len - len(cur_line[len(cur_line) - 1])
                        cur_line.pop()
                    if not placed:
                        if len(lines) > 0:
                            prev_line: str = lines[len(lines) - 1].rstrip()
                            if (len(prev_line) + len(self.placeholder)
                                    <= self.width):
                                lines[len(lines) - 1] = (prev_line
                                                         + self.placeholder)
                                placed = True
                        if not placed:
                            lines.append(indent + self.placeholder.lstrip())
                    break
            elif _total_len(rev) == remaining:
                # CPython 3.14 在「indent 比 width 宽 + 文本以空白开头」时
                # 会死循环（rev 尾部变成空块、不再前进）。这里检测到无进展
                # 就收手，返回已排好的行，不死循环（见模块头边界说明）。
                break
        return lines

    def _split_chunks(self, text: str) -> list[str]:
        return self._split(self._munge_whitespace(text))

    def wrap(self, text: str) -> list[str]:
        chunks: list[str] = self._split_chunks(text)
        if self.fix_sentence_endings:
            self._fix_sentence_endings(chunks)
        return self._wrap_chunks(chunks)

    def fill(self, text: str) -> str:
        return "\n".join(self.wrap(text))


def wrap(text: str, width: int = 70, initial_indent: str = "",
         subsequent_indent: str = "", expand_tabs: bool = True,
         replace_whitespace: bool = True, fix_sentence_endings: bool = False,
         break_long_words: bool = True, drop_whitespace: bool = True,
         break_on_hyphens: bool = True, tabsize: int = 8,
         max_lines: int = None, placeholder: str = " [...]") -> list[str]:
    """折行，返回行列表（参数见 TextWrapper）。"""
    w = TextWrapper(width, initial_indent, subsequent_indent, expand_tabs,
                    replace_whitespace, fix_sentence_endings,
                    break_long_words, drop_whitespace, break_on_hyphens,
                    tabsize, max_lines, placeholder)
    return w.wrap(text)


def fill(text: str, width: int = 70, initial_indent: str = "",
         subsequent_indent: str = "", expand_tabs: bool = True,
         replace_whitespace: bool = True, fix_sentence_endings: bool = False,
         break_long_words: bool = True, drop_whitespace: bool = True,
         break_on_hyphens: bool = True, tabsize: int = 8,
         max_lines: int = None, placeholder: str = " [...]") -> str:
    """折行并用换行符连接（参数见 TextWrapper）。"""
    w = TextWrapper(width, initial_indent, subsequent_indent, expand_tabs,
                    replace_whitespace, fix_sentence_endings,
                    break_long_words, drop_whitespace, break_on_hyphens,
                    tabsize, max_lines, placeholder)
    return w.fill(text)


def shorten(text: str, width: int, placeholder: str = " [...]",
            initial_indent: str = "", subsequent_indent: str = "",
            expand_tabs: bool = True, replace_whitespace: bool = True,
            fix_sentence_endings: bool = False, break_long_words: bool = True,
            drop_whitespace: bool = True, break_on_hyphens: bool = True,
            tabsize: int = 8) -> str:
    """压空白后截断到 width，放不下时用 placeholder 收尾。

    参数顺序：CPython 的这些参数是 keyword-only（位置调用会 TypeError），
    pa 侧把 placeholder 放在第三位以保持旧 pa 的 shorten(text, width, "...")
    调用可用；关键字调用与 CPython 逐字一致。
    """
    w = TextWrapper(width, initial_indent, subsequent_indent, expand_tabs,
                    replace_whitespace, fix_sentence_endings,
                    break_long_words, drop_whitespace, break_on_hyphens,
                    tabsize, 1, placeholder)
    return w.fill(" ".join(text.strip().split()))


def dedent(text: str) -> str:
    """去掉所有非空行共有的前导空白；纯空白行归一成空串。"""
    lines: list[str] = text.split("\n")
    non_blank: list[str] = []
    for line in lines:
        if len(line) > 0 and not line.isspace():
            non_blank.append(line)
    margin: int = 0
    if len(non_blank) > 0:
        l1: str = non_blank[0]
        l2: str = non_blank[0]
        for line in non_blank:
            if line < l1:
                l1 = line
            if line > l2:
                l2 = line
        i: int = 0
        while i < len(l1):
            c: str = l1[i]
            if c != l2[i] or (c != " " and c != "\t"):
                break
            margin = i + 1
            i = i + 1
    out: list[str] = []
    for line in lines:
        if line.isspace():
            out.append("")
        else:
            out.append(line[margin:])
    return "\n".join(out)


def indent(text: str, prefix: str, predicate: object = None) -> str:
    """给选中的行加前缀：默认给所有非纯空白的行加（含行尾换行符）。"""
    out: list[str] = []
    for line in _splitlines_keepends(text):
        if predicate is None:
            if not line.isspace():
                out.append(prefix)
        elif predicate(line):
            out.append(prefix)
        out.append(line)
    return "".join(out)
