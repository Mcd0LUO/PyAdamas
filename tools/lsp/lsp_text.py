# tools/lsp/lsp_text.py
# LSP 的文本层：小工具、UTF-16 位置换算、定义行查找。
# 不依赖其他 lsp_* 模块；不做类型推断 / 作用域分析。

# ---------------- 小工具 ----------------

def is_digit_str(s: str) -> bool:
    if s == "":
        return False
    i: int = 0
    while i < len(s):
        o: int = ord(s[i])
        if o < 48 or o > 57:
            return False
        i = i + 1
    return True

def first_non_space(s: str, start: int) -> int:
    i: int = start
    while i < len(s):
        c: str = s[i]
        if c == " " or c == "\t":
            i = i + 1
        else:
            return i
    return i

def is_ident_char(c: str) -> bool:
    o: int = ord(c)
    if o >= 65 and o <= 90:
        return True
    if o >= 97 and o <= 122:
        return True
    if o >= 48 and o <= 57:
        return True
    return o == 95

def read_ident(s: str, start: int) -> str:
    i: int = first_non_space(s, start)
    begin: int = i
    while i < len(s) and is_ident_char(s[i]):
        i = i + 1
    return s[begin:i]

def indent_width(line: str) -> int:
    width: int = 0
    i: int = 0
    while i < len(line):
        c: str = line[i]
        if c == " ":
            width = width + 1
        elif c == "\t":
            width = width + 8
        else:
            return width
        i = i + 1
    return width

def truncate(s: str, n: int) -> str:
    if len(s) <= n:
        return s
    return s[:n] + " ..."

def strip_trailing_colon(s: str) -> str:
    if len(s) > 0 and s[len(s) - 1] == ":":
        return s[:len(s) - 1]
    return s

def utf16_units(c: str) -> int:
    if ord(c) > 0xFFFF:
        return 2
    return 1

def cp_index_at(line: str, character: int) -> int:
    # LSP position 的 character 按 UTF-16 code unit 计；转成码点下标
    i: int = 0
    units: int = 0
    while i < len(line) and units < character:
        units = units + utf16_units(line[i])
        i = i + 1
    return i

def offset_at(text: str, line: int, character: int) -> int:
    # (line, character[UTF-16 units]) -> 码点偏移；越界时夹到行尾/文末
    line_no: int = 0
    i: int = 0
    while i < len(text) and line_no < line:
        if text[i] == "\n":
            line_no = line_no + 1
        i = i + 1
    units: int = 0
    while i < len(text) and text[i] != "\n" and units < character:
        units = units + utf16_units(text[i])
        i = i + 1
    return i

def word_at(text: str, line: int, character: int) -> str:
    lines: list[str] = text.split("\n")
    if line < 0 or line >= len(lines):
        return ""
    raw: str = lines[line]
    idx: int = cp_index_at(raw, character)
    start: int = idx
    while start > 0 and is_ident_char(raw[start - 1]):
        start = start - 1
    end: int = idx
    while end < len(raw) and is_ident_char(raw[end]):
        end = end + 1
    if end <= start:
        return ""
    return raw[start:end]

def params_text(sig: str) -> str:
    p1: int = sig.find("(")
    p2: int = sig.find(")")
    if p1 < 0 or p2 < 0 or p2 <= p1:
        return ""
    return sig[p1 + 1:p2]

def is_param(params: str, word: str) -> bool:
    parts: list[str] = params.split(",")
    i: int = 0
    while i < len(parts):
        p: str = parts[i].strip()
        cut: int = len(p)
        c1: int = p.find(":")
        c2: int = p.find("=")
        if c1 >= 0 and c1 < cut:
            cut = c1
        if c2 >= 0 and c2 < cut:
            cut = c2
        nm: str = p[:cut].strip()
        while len(nm) > 0 and (nm[0] == "*" or nm[0] == "/"):
            nm = nm[1:]
        if nm == word:
            return True
        i = i + 1
    return False

def find_def_line(text: str, word: str) -> str:
    lines: list[str] = text.split("\n")
    i: int = 0
    while i < len(lines):
        stripped: str = lines[i].strip()
        if stripped.startswith("async def "):
            sig: str = strip_trailing_colon(stripped)
            if read_ident(stripped, len("async def ")) == word:
                return sig
            if is_param(params_text(sig), word):
                return sig
        elif stripped.startswith("def "):
            sig2: str = strip_trailing_colon(stripped)
            if read_ident(stripped, len("def ")) == word:
                return sig2
            if is_param(params_text(sig2), word):
                return sig2
        elif stripped.startswith("class "):
            if read_ident(stripped, len("class ")) == word:
                return strip_trailing_colon(stripped)
        else:
            nm: str = read_ident(stripped, 0)
            if nm == word:
                pos: int = first_non_space(stripped, len(nm))
                if pos < len(stripped):
                    c: str = stripped[pos]
                    if c == ":" or c == "=":
                        return truncate(stripped, 160)
        i = i + 1
    return ""

def leading_ws(raw: str) -> int:
    i: int = 0
    while i < len(raw) and (raw[i] == " " or raw[i] == "\t"):
        i = i + 1
    return i

def find_def_location(text: str, word: str) -> list[int]:
    # 返回 [line, character]（UTF-16 列，标识符都是 ASCII 所以等于码点数），
    # 找不到返回 []。覆盖：任意缩进的 def/async def/class/方法、顶层赋值、
    # 以及「光标在形参上 -> 跳到所在函数定义」。
    lines: list[str] = text.split("\n")
    i: int = 0
    while i < len(lines):
        raw: str = lines[i]
        indent: int = leading_ws(raw)
        stripped: str = raw[indent:]
        # 形参命中：先匹配函数名，再匹配形参名（跳函数定义）
        if stripped.startswith("async def ") or stripped.startswith("def "):
            off: int = len("async def ")
            if not stripped.startswith("async def "):
                off = len("def ")
            fname: str = read_ident(stripped, off)
            sig: str = strip_trailing_colon(stripped)
            if fname == word:
                return [i, indent + off]
            if is_param(params_text(sig), word) and fname != "":
                return [i, indent + off]
        elif stripped.startswith("class "):
            cname: str = read_ident(stripped, len("class "))
            if cname == word:
                return [i, indent + len("class ")]
        elif indent == 0:
            nm: str = read_ident(stripped, 0)
            if nm == word:
                pos: int = first_non_space(stripped, len(nm))
                if pos < len(stripped) and (stripped[pos] == ":" or stripped[pos] == "="):
                    return [i, 0]
        i = i + 1
    return []

def u16_prefix_len(s: str, idx: int) -> int:
    # 码点下标 -> UTF-16 code unit 数（LSP position 的口径）。
    units: int = 0
    i: int = 0
    while i < idx and i < len(s):
        units = units + utf16_units(s[i])
        i = i + 1
    return units

def is_digit_char(c: str) -> bool:
    return c >= "0" and c <= "9"

def scan_ident_end(s: str, start: int) -> int:
    i: int = start
    while i < len(s) and is_ident_char(s[i]):
        i = i + 1
    return i

PY_KEYWORDS: list[str] = [
    "False", "None", "True", "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else", "except", "finally",
    "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
    "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
]

def list_has(xs: list[str], s: str) -> bool:
    i: int = 0
    while i < len(xs):
        if xs[i] == s:
            return True
        i = i + 1
    return False

def is_py_keyword(w: str) -> bool:
    return list_has(PY_KEYWORDS, w)
