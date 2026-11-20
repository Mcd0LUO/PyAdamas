"""pprint —— 数据结构的递归格式化打印（纯 pyadamas 源码，CPython 3.14 口径）。

覆盖 pformat / pprint / PrettyPrinter，参数面与 CPython 一致：
indent（每层缩进，>=0）、width（目标列宽，!=0）、depth（最大层数，-1=不限，即 CPython 的 None）、
compact（列表/元组/集合的紧凑排布）、sort_dicts（**默认 True**：dict 键排序后
输出；False 按插入顺序）。空容器与「单行就放得下」的对象走 CPython 的
_safe_repr 口径（dict 按 sort_dicts 排序、set 排序、单元素 tuple 尾逗号），
放不下才按容器类型断行（缩进 + indent_per_level、每项 allowance=1 给尾逗号
留位、dict 键值对齐、compact 的宽度预算、长字符串按空白折行）。

错误口径：indent < 0 → ValueError("indent must be >= 0")；depth <= 0 且不是 -1
哨兵 → ValueError("depth must be > 0")；width == 0 → ValueError("width must be != 0")。

语义边界（docs/type-tags-landing.md §5，与运行期类型标签同源）：
- 分派靠 isinstance/repr 的运行期类型码，需要 PYADAMAS_TYPE_TAGS=1；关时退化
  成 0.339 的探针口径。
- **容器元素没有逐元素 tag**（容器只存 8B 值）：嵌套的 bool/None 会显示成
  int 的 0/1、tuple→list、set→dict、bytes→str。顶层值与静态类型已知的容器
  元素不受影响。
- **没有递归检测**：运行期没有 id()，自引用容器会无限递归（CPython 会打成
  <Recursion on ...>/{...}）。纯 pa 侧做不了，属负面。
- dict 键排序：同类型键按自然序；混类型键先比类型名再比 repr（CPython 的
  _safe_key 同类型名时用 id 打破平局，地址序不可复现，故用 repr 兜底）。
- 长字符串折行按 ASCII 空白（CPython 的 \\S/\\s 对 str 是 Unicode 语义）；
  bytes/bytearray 不折行（CPython 会按长度切）。
- depth 用 -1 表示 CPython 的 None（可空 int 槽里 None 与 0 同形，用 None
  默认值会把 depth=0 误当「不限」；显式传 None 也会被静态类型检查拒绝）。
  其余 depth<=0 与 CPython 一样报 ValueError("depth must be > 0")。
- saferepr / isreadable / isrecursive / pp 未提供（需要递归检测与 readable 标志）。
"""

_W: int = 80
_IPL: int = 1
_DEPTH: int = 0
_COMPACT: bool = False
_SORT: bool = True


def _spaces(n: int) -> str:
    """n 个空格的缩进串。"""
    s: str = ""
    i: int = 0
    while i < n:
        s = s + " "
        i = i + 1
    return s


def _is_space(c: str) -> bool:
    return (c == " " or c == "\t" or c == "\n" or c == "\x0b"
            or c == "\x0c" or c == "\r")


def _splitlines_keepends(text: str) -> list[str]:
    """str.splitlines(True) 的 ASCII 子集（长字符串折行用）。"""
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


def _type_rank(x: object) -> str:
    """CPython _safe_key 里的 str(type(obj))：mixed 键排序的第一关键字。"""
    if isinstance(x, bool):
        return "<class 'bool'>"
    if isinstance(x, int):
        return "<class 'int'>"
    if isinstance(x, float):
        return "<class 'float'>"
    if isinstance(x, str):
        return "<class 'str'>"
    if isinstance(x, bytes):
        return "<class 'bytes'>"
    if isinstance(x, list):
        return "<class 'list'>"
    if isinstance(x, tuple):
        return "<class 'tuple'>"
    if isinstance(x, dict):
        return "<class 'dict'>"
    if isinstance(x, set):
        return "<class 'set'>"
    if x is None:
        return "<class 'NoneType'>"
    return "<class 'object'>"


def _safe_lt(a: object, b: object) -> bool:
    """可比就比较；不可比（混类型）按 (类型名, repr) 排（CPython 的 Py2 式兜底）。"""
    try:
        if a < b:
            return True
        if b < a:
            return False
    except TypeError:
        pass
    ra: str = _type_rank(a)
    rb: str = _type_rank(b)
    if ra != rb:
        return ra < rb
    return repr(a) < repr(b)


def _merge_sort(items: list[object]) -> list[object]:
    """稳定归并排序（不依赖 sorted() 的 unknown 比较路径）。"""
    n: int = len(items)
    if n <= 1:
        return items
    mid: int = n // 2
    left: list[object] = _merge_sort(items[:mid])
    right: list[object] = _merge_sort(items[mid:])
    out: list[object] = []
    i: int = 0
    j: int = 0
    while i < len(left) and j < len(right):
        if _safe_lt(right[j], left[i]):
            out.append(right[j])
            j = j + 1
        else:
            out.append(left[i])
            i = i + 1
    while i < len(left):
        out.append(left[i])
        i = i + 1
    while j < len(right):
        out.append(right[j])
        j = j + 1
    return out


def _items_of(obj: object) -> list[object]:
    """unknown 容器的元素列表（list/tuple/set 都支持 for 迭代）。"""
    items: list[object] = []
    for x in obj:
        items.append(x)
    return items


def _pairs_of(obj: object) -> list[object]:
    """unknown dict 的 (key, value) 对列表（按插入顺序）。"""
    return __pa_any_items(obj)


def _sorted_items(items: list[object]) -> list[object]:
    return _merge_sort(items)


def _safe_repr(obj: object, level: int) -> str:
    """CPython _safe_repr 的单行口径（sort_dicts / set 排序 / 深度截断）。"""
    if isinstance(obj, dict):
        pairs: list[object] = _pairs_of(obj)
        if len(pairs) == 0:
            return "{}"
        if _DEPTH > 0 and level >= _DEPTH:
            return "{...}"
        if _SORT and len(pairs) > 1:
            pairs = _sorted_by_key(pairs)
        out: str = "{"
        i: int = 0
        for p in pairs:
            if i > 0:
                out = out + ", "
            out = out + _safe_repr(p[0], level + 1) + ": " + _safe_repr(p[1], level + 1)
            i = i + 1
        return out + "}"
    if isinstance(obj, list):
        items: list[object] = _items_of(obj)
        if len(items) == 0:
            return "[]"
        if _DEPTH > 0 and level >= _DEPTH:
            return "[...]"
        out = "["
        i = 0
        for x in items:
            if i > 0:
                out = out + ", "
            out = out + _safe_repr(x, level + 1)
            i = i + 1
        return out + "]"
    if isinstance(obj, tuple):
        items = _items_of(obj)
        if len(items) == 0:
            return "()"
        if _DEPTH > 0 and level >= _DEPTH:
            return "(...)"
        out = "("
        i = 0
        for x in items:
            if i > 0:
                out = out + ", "
            out = out + _safe_repr(x, level + 1)
            i = i + 1
        if len(items) == 1:
            out = out + ","
        return out + ")"
    if isinstance(obj, set):
        items = _items_of(obj)
        if len(items) == 0:
            return "set()"
        items = _sorted_items(items)
        out = "{"
        i = 0
        for x in items:
            if i > 0:
                out = out + ", "
            out = out + _safe_repr(x, level)
            i = i + 1
        return out + "}"
    return repr(obj)


def _sorted_by_key(pairs: list[object]) -> list[object]:
    """按 p[0] 稳定排序的键值对（归并排序，混类型走 _safe_lt 兜底）。"""
    n: int = len(pairs)
    if n <= 1:
        return pairs
    mid: int = n // 2
    left: list[object] = _sorted_by_key(pairs[:mid])
    right: list[object] = _sorted_by_key(pairs[mid:])
    out: list[object] = []
    i: int = 0
    j: int = 0
    while i < len(left) and j < len(right):
        if _safe_lt(right[j][0], left[i][0]):
            out.append(right[j])
            j = j + 1
        else:
            out.append(left[i])
            i = i + 1
    while i < len(left):
        out.append(left[i])
        i = i + 1
    while j < len(right):
        out.append(right[j])
        j = j + 1
    return out


def _format(obj: object, indent: int, allowance: int, level: int) -> str:
    """单行 repr 放得下就用它，否则按容器类型断行（CPython 口径）。"""
    rep: str = _safe_repr(obj, level)
    if len(rep) <= _W - indent - allowance:
        return rep
    if isinstance(obj, dict):
        return _dict_text(obj, indent, allowance, level + 1)
    if isinstance(obj, list):
        return _seq_text(obj, "[", "]", indent, allowance + 1, level + 1)
    if isinstance(obj, tuple):
        items: list[object] = _items_of(obj)
        if len(items) == 1:
            return _seq_items_text(items, "(", ",)", indent, allowance + 2, level + 1)
        return _seq_text(obj, "(", ")", indent, allowance + 1, level + 1)
    if isinstance(obj, set):
        return _set_text(obj, indent, allowance + 1, level + 1)
    if isinstance(obj, str):
        return _str_text(obj, indent, allowance, level + 1)
    return rep


def _dict_text(obj: object, indent: int, allowance: int, level: int) -> str:
    """dict 的断行排版：每行一个 key: value，续行缩进 indent+IPL。"""
    out: str = "{"
    if _IPL > 1:
        out = out + _spaces(_IPL - 1)
    pairs: list[object] = _pairs_of(obj)
    if len(pairs) > 1 and _SORT:
        pairs = _sorted_by_key(pairs)
    if len(pairs) > 0:
        out = out + _dict_items_text(pairs, indent, allowance + 1, level)
    return out + "}"


def _dict_items_text(pairs: list[object], indent: int, allowance: int,
                     level: int) -> str:
    indent2: int = indent + _IPL
    delimnl: str = ",\n" + _spaces(indent2)
    out: str = ""
    last_index: int = len(pairs) - 1
    i: int = 0
    while i <= last_index:
        p: object = pairs[i]
        last: bool = i == last_index
        krepr: str = _safe_repr(p[0], level)
        out = out + krepr + ": "
        out = out + _format(p[1], indent2 + len(krepr) + 2,
                            allowance if last else 1, level)
        if not last:
            out = out + delimnl
        i = i + 1
    return out


def _set_text(obj: object, indent: int, allowance: int, level: int) -> str:
    items: list[object] = _items_of(obj)
    if len(items) == 0:
        return repr(obj)
    items = _sorted_items(items)
    return _seq_items_text(items, "{", "}", indent, allowance, level)


def _seq_text(obj: object, opener: str, closer: str, indent: int,
              allowance: int, level: int) -> str:
    items: list[object] = _items_of(obj)
    return _seq_items_text(items, opener, closer, indent, allowance, level)


def _seq_items_text(items: list[object], opener: str, closer: str, indent: int,
                    allowance: int, level: int) -> str:
    out: str = opener
    if _IPL > 1:
        out = out + _spaces(_IPL - 1)
    indent2: int = indent + _IPL
    delimnl: str = ",\n" + _spaces(indent2)
    delim: str = ""
    n: int = len(items)
    if n == 0:
        # CPython 的 _format_items 先写 (indent_per_level-1) 个空格，再发现
        # 没有元素就返回——空容器被派发时是 "[" + 空格 + "]"（indent>1 才有空格）。
        return out + closer
    width: int = _W - indent2 + 1
    max_width: int = width
    i: int = 0
    last: bool = False
    while not last:
        ent: object = items[i]
        i = i + 1
        if i >= n:
            last = True
            width = width - allowance
            max_width = max_width - allowance
        if _COMPACT:
            rep: str = _safe_repr(ent, level)
            w: int = len(rep) + 2
            if width < w:
                width = max_width
                if delim != "":
                    delim = delimnl
            if width >= w:
                width = width - w
                out = out + delim
                delim = ", "
                out = out + rep
                continue
        out = out + delim
        delim = delimnl
        out = out + _format(ent, indent2, allowance if last else 1, level)
    return out + closer


def _parts(line: str) -> list[str]:
    """CPython re.findall(r'\\S*\\s*', line) 的等价切分（末尾带空串）。"""
    parts: list[str] = []
    n: int = len(line)
    i: int = 0
    while i < n:
        j: int = i
        while j < n and not _is_space(line[j]):
            j = j + 1
        while j < n and _is_space(line[j]):
            j = j + 1
        parts.append(line[i:j])
        i = j
    parts.append("")
    return parts


def _str_text(s: str, indent: int, allowance: int, level: int) -> str:
    """长字符串折行（CPython _pprint_str 口径）。"""
    if len(s) == 0:
        return repr(s)
    chunks: list[str] = []
    lines: list[str] = _splitlines_keepends(s)
    if level == 1:
        indent = indent + 1
        allowance = allowance + 1
    max_width: int = _W - indent
    max_width1: int = max_width
    rep: str = repr(s)
    i: int = 0
    while i < len(lines):
        line: str = lines[i]
        rep = repr(line)
        if i == len(lines) - 1:
            max_width1 = max_width1 - allowance
        if len(rep) <= max_width1:
            chunks.append(rep)
        else:
            parts: list[str] = _parts(line)
            if len(parts) > 0 and parts[len(parts) - 1] == "":
                parts.pop()
            max_width2: int = max_width
            current: str = ""
            j: int = 0
            while j < len(parts):
                part: str = parts[j]
                candidate: str = current + part
                if j == len(parts) - 1 and i == len(lines) - 1:
                    max_width2 = max_width2 - allowance
                if len(repr(candidate)) > max_width2:
                    if current != "":
                        chunks.append(repr(current))
                    current = part
                else:
                    current = candidate
                j = j + 1
            if current != "":
                chunks.append(repr(current))
        i = i + 1
    if len(chunks) == 1:
        return rep
    out: str = ""
    if level == 1:
        out = "("
    i = 0
    while i < len(chunks):
        if i > 0:
            out = out + "\n" + _spaces(indent)
        out = out + chunks[i]
        i = i + 1
    if level == 1:
        out = out + ")"
    return out


def _setup(indent: int, width: int, depth: int, compact: bool,
           sort_dicts: bool) -> None:
    if indent < 0:
        raise ValueError("indent must be >= 0")
    if depth != -1 and depth <= 0:
        # CPython 的 depth=None（不限）在 pa 用 -1 哨兵表示：可空 int 槽里
        # None 与 0 同形，用 None 默认值会把 depth=0 误当「不限」。
        raise ValueError("depth must be > 0")
    if width == 0:
        raise ValueError("width must be != 0")
    global _W, _IPL, _DEPTH, _COMPACT, _SORT
    _W = width
    _IPL = indent
    _DEPTH = depth
    _COMPACT = compact
    _SORT = sort_dicts


def pformat(obj: object, indent: int = 1, width: int = 80, depth: int = -1,
            compact: bool = False, sort_dicts: bool = True) -> str:
    """把 obj 格式化成字符串（参数口径见模块头）。"""
    _setup(indent, width, depth, compact, sort_dicts)
    return _format(obj, 0, 0, 0)


def pprint(obj: object, indent: int = 1, width: int = 80, depth: int = -1,
           compact: bool = False, sort_dicts: bool = True) -> None:
    """pformat 之后写到标准输出（带换行）。"""
    print(pformat(obj, indent, width, depth, compact, sort_dicts))


class PrettyPrinter:
    """CPython PrettyPrinter 的子集（无 stream 参数：pa 侧直接写 stdout）。"""

    indent: int
    width: int
    depth: int
    compact: bool
    sort_dicts: bool

    def __init__(self, indent: int = 1, width: int = 80, depth: int = -1,
                 compact: bool = False, sort_dicts: bool = True) -> None:
        # CPython 在构造期就校验参数（不是等到 pformat）；这里复用 _setup 的
        # 校验与状态设置，错误类型/消息一致。
        _setup(indent, width, depth, compact, sort_dicts)
        self.indent = indent
        self.width = width
        self.depth = depth
        self.compact = compact
        self.sort_dicts = sort_dicts

    def pformat(self, obj: object) -> str:
        return pformat(obj, self.indent, self.width, self.depth, self.compact,
                       self.sort_dicts)

    def pprint(self, obj: object) -> None:
        print(self.pformat(obj))
