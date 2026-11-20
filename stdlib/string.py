"""string —— 字符串常量、Template 与 capwords（纯 pyadamas 源码）。

覆盖 CPython string 的公开面里能纯源码实现的部分：
- 常量：ascii_lowercase / ascii_uppercase / ascii_letters / digits /
  hexdigits / octdigits / punctuation / whitespace / printable（逐字节相同）。
- Template：$identifier、${identifier}、$$ 转义、substitute（缺键 KeyError）/
  safe_substitute（缺键原样保留）/ is_valid / get_identifiers；非法占位符
  ValueError（"Invalid placeholder in string: line N, col M"，行列口径与
  CPython 一致，行分隔按 ASCII 子集）。
- capwords(s, sep=None)：空白归一 + 每词 capitalize + 单空格连接；给 sep 时
  按 sep 切/接；sep="" 与 CPython 一样抛 ValueError("empty separator")。

语义边界（纯 pyadamas 表示所限）：
- Template 只支持默认的 delimiter="$" / idpattern（ASCII 标识符，IGNORECASE）
  与 braceidpattern=None；子类改 delimiter/idpattern/flags 不生效。
- substitute/safe_substitute 只收一个 mapping 实参（CPython 的 **kws 形式
  不支持）；mapping 需是 dict（走 __pa_any_items 线性查找）。
- 查到的值用 str() 转换。受 P2「容器元素没有逐元素 tag」限制，从 mapping
  取出的 8B 值只剩位模式：str 值与 int 值转换正确；float 值会打成 8B 位
  整数、None/False 打成 "0"、True 打成 "1"、list/dict/tuple 值打成指针。
  要与 CPython 输出一致，Template 的取值请用 str（或同构 int）。这是运行期
  容器 tag 缺口（见 docs/type-tags-landing.md），不是本模块的选择。
- get_identifiers 返回的顺序去重列表（CPython 同）。
- Formatter / _string 的 format 解析面不做（需要运行期 format 协议）。
"""

ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = digits + "abcdef" + "ABCDEF"
octdigits = "01234567"
punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace = " \t\n\r\x0b\x0c"
printable = digits + ascii_letters + punctuation + whitespace


def _is_id_start(c: str) -> bool:
    """Template 标识符首字符（(?a:[_a-z]) 加 IGNORECASE）。"""
    return ((c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or c == "_")


def _is_id_char(c: str) -> bool:
    """Template 标识符后续字符（(?a:[_a-z0-9]) 加 IGNORECASE）。"""
    return _is_id_start(c) or (c >= "0" and c <= "9")


def _splitlines_keepends(text: str) -> list[str]:
    """str.splitlines(True) 的 ASCII 子集（供 ValueError 行列计算）。"""
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


def _invalid(template: str, pos: int) -> None:
    """按 CPython Template._invalid 的口径报 ValueError（pos 是占位符后的位置）。"""
    lines: list[str] = _splitlines_keepends(template[:pos])
    lineno: int = 1
    colno: int = 1
    if len(lines) > 0:
        before: str = ""
        i: int = 0
        while i < len(lines) - 1:
            before = before + lines[i]
            i = i + 1
        colno = pos - len(before)
        lineno = len(lines)
    raise ValueError("Invalid placeholder in string: line " + str(lineno)
                     + ", col " + str(colno))


class Template:
    """支持 $ 替换的字符串模板（CPython string.Template 子集）。"""

    delimiter: str
    template: str

    def __init__(self, template: str) -> None:
        self.template = template

    def _invalid_at(self, pos: int) -> None:
        _invalid(self.template, pos)

    def substitute(self, mapping: object = None) -> str:
        """替换所有占位符；缺键抛 KeyError(name)，非法占位符抛 ValueError。"""
        return _substitute(self, mapping, True)

    def safe_substitute(self, mapping: object = None) -> str:
        """同 substitute，但缺键/非法占位符按原文保留。"""
        return _substitute(self, mapping, False)

    def is_valid(self) -> bool:
        """模板里是否没有非法占位符。"""
        text: str = self.template
        n: int = len(text)
        i: int = 0
        while i < n:
            if text[i] != "$":
                i = i + 1
                continue
            if i + 1 < n and text[i + 1] == "$":
                i = i + 2
                continue
            stop: int = _placeholder_end(text, i)
            if stop < 0:
                return False
            i = stop
        return True

    def get_identifiers(self) -> list[str]:
        """模板引用的标识符（按首次出现顺序去重）。"""
        text: str = self.template
        n: int = len(text)
        i: int = 0
        ids: list[str] = []
        while i < n:
            if text[i] != "$":
                i = i + 1
                continue
            if i + 1 < n and text[i + 1] == "$":
                i = i + 2
                continue
            stop: int = _placeholder_end(text, i)
            if stop < 0:
                i = i + 1
                continue
            name: str = _placeholder_name(text, i)
            if name != "" and name not in ids:
                ids.append(name)
            i = stop
        return ids


def _placeholder_name(text: str, i: int) -> str:
    """text[i] == "$" 且占位符合法时返回标识符名，否则空串。"""
    n: int = len(text)
    j: int = i + 1
    if j < n and text[j] == "{":
        k: int = j + 1
        start: int = k
        if k < n and _is_id_start(text[k]):
            k = k + 1
            while k < n and _is_id_char(text[k]):
                k = k + 1
            if k < n and text[k] == "}":
                return text[start:k]
        return ""
    start = j
    if j < n and _is_id_start(text[j]):
        k = j + 1
        while k < n and _is_id_char(text[k]):
            k = k + 1
        return text[start:k]
    return ""


def _placeholder_end(text: str, i: int) -> int:
    """text[i] == "$" 的合法占位符结束位置；非法返回 -1（$$ 返回 i+2）。"""
    n: int = len(text)
    if i + 1 < n and text[i + 1] == "$":
        return i + 2
    j: int = i + 1
    if j < n and text[j] == "{":
        k: int = j + 1
        if k < n and _is_id_start(text[k]):
            k = k + 1
            while k < n and _is_id_char(text[k]):
                k = k + 1
            if k < n and text[k] == "}":
                return k + 1
        return -1
    if j < n and _is_id_start(text[j]):
        k = j + 1
        while k < n and _is_id_char(text[k]):
            k = k + 1
        return k
    return -1


def _substitute(tpl: Template, mapping: object, strict: bool) -> str:
    text: str = tpl.template
    n: int = len(text)
    out: str = ""
    i: int = 0
    while i < n:
        c: str = text[i]
        if c != "$":
            out = out + c
            i = i + 1
            continue
        if i + 1 < n and text[i + 1] == "$":
            out = out + "$"
            i = i + 2
            continue
        name: str = _placeholder_name(text, i)
        if name == "":
            if strict:
                tpl._invalid_at(i + 1)
            out = out + "$"
            i = i + 1
            continue
        stop: int = _placeholder_end(text, i)
        value: object = None
        found: bool = False
        if mapping is not None:
            pairs: list[object] = __pa_any_items(mapping)
            for p in pairs:
                if p[0] == name:
                    value = p[1]
                    found = True
                    break
        if found:
            out = out + str(value)
            i = stop
            continue
        if strict:
            raise KeyError(name)
        out = out + text[i:stop]
        i = stop
    return out


def capwords(s: str, sep: str = None) -> str:
    """按空白（或 sep）切词，每词 capitalize 后用一个空格（或 sep）连接。"""
    words: list[str] = []
    joiner: str = " "
    if sep is None:
        words = s.split()
    else:
        words = s.split(sep)
        joiner = sep
    out: str = ""
    first: bool = True
    for w in words:
        if not first:
            out = out + joiner
        out = out + w.capitalize()
        first = False
    return out
