"""pprint —— 数据结构的递归格式化打印（纯 pyadamas 源码，P2 类型标签）。

覆盖：pformat / pprint / PrettyPrinter。按「单行 repr 放得下就用它，放不下
才递归断行」的 CPython 口径排版（缩进、dict 键值对齐、单元素 tuple 尾逗号）。

语义边界（docs/numeric-types-design.md §2，与运行期类型标签同源）：
- 分派靠 isinstance/repr 的运行期类型码。静态类型已知的 unknown 边界
  （函数形参、object 槽）能精确区分 None/bool/int/float/str/bytes/list/
  dict/tuple/set（需 PYADAMAS_TYPE_TAGS=1；关时退化为 0.339 的探针口径）。
- 容器元素没有逐元素 tag（容器只存 8B 值）：嵌套的 bool/None/tuple/set/
  bytes 会退化成 int/list/dict/str。顶层值与静态类型已知的容器元素不受
  影响（例如 list[str] 的元素）。
- 不排序 dict 键：CPython 默认 sort_dicts=True（按 key 排序），这里按插入
  顺序输出。需要 CPython 顺序时用键已排好序的字典字面量。
- 宽度按码点数计（str 的 len 是码点数），非 ASCII 与 CPython 一致。
"""


def _spaces(n: int) -> str:
    """n 个空格的缩进串。"""
    s: str = ""
    i: int = 0
    while i < n:
        s = s + " "
        i = i + 1
    return s


def _seq_text(obj: object, opener: str, closer: str, col: int, width: int,
              depth: int) -> str:
    """list/tuple/set 的断行排版。元素逐个取（unknown 容器按运行期种类迭代）。"""
    items: list[object] = []
    for x in obj:
        items.append(x)
    if len(items) == 0:
        return opener + closer
    ncol: int = col + 1
    out: str = opener
    i: int = 0
    for x in items:
        if i > 0:
            out = out + ",\n" + _spaces(ncol)
        out = out + _format(x, ncol, width, depth - 1)
        i = i + 1
    if closer == ")" and len(items) == 1:
        out = out + ","
    out = out + closer
    return out


def _dict_text(obj: object, col: int, width: int, depth: int) -> str:
    """dict 的断行排版：每行一个 key: value，续行缩进 col+1，值从键后对齐。"""
    pairs: list[object] = __pa_any_items(obj)
    if len(pairs) == 0:
        return "{}"
    ncol: int = col + 1
    out: str = "{"
    i: int = 0
    for p in pairs:
        if i > 0:
            out = out + ",\n" + _spaces(ncol)
        k: object = p[0]
        v: object = p[1]
        kr: str = repr(k)
        out = out + kr + ": "
        vcol: int = ncol + len(kr) + 2
        out = out + _format(v, vcol, width, depth - 1)
        i = i + 1
    out = out + "}"
    return out


def _format(obj: object, col: int, width: int, depth: int) -> str:
    """单行 repr 放得下就用它，否则按容器类型递归断行（CPython 口径）。"""
    s: str = repr(obj)
    if len(s) <= width - col:
        return s
    if depth == 0:
        if isinstance(obj, dict):
            return "{...}"
        if isinstance(obj, list):
            return "[...]"
        if isinstance(obj, tuple):
            return "(...)"
        if isinstance(obj, set):
            return "{...}"
        return s
    if isinstance(obj, dict):
        return _dict_text(obj, col, width, depth)
    if isinstance(obj, list):
        return _seq_text(obj, "[", "]", col, width, depth)
    if isinstance(obj, tuple):
        return _seq_text(obj, "(", ")", col, width, depth)
    if isinstance(obj, set):
        return _seq_text(obj, "{", "}", col, width, depth)
    return s


def pformat(obj: object, indent: int = 1, width: int = 80,
            depth: int = -1) -> str:
    """把 obj 格式化成字符串。indent 目前固定为 1（CPython 默认口径）；
    depth=-1 表示不限层数，depth=0 表示容器只打 {...} / [...]。"""
    if indent > 1:
        # CPython 的 indent 参数影响续行缩进；这里只支持默认 1。
        # 显式报错而不是静默给出不同格式。
        raise ValueError("pprint: only indent=1 is supported")
    return _format(obj, 0, width, depth)


def pprint(obj: object, indent: int = 1, width: int = 80,
           depth: int = -1) -> None:
    """pformat 之后写到标准输出（带换行）。"""
    print(pformat(obj, indent, width, depth))


class PrettyPrinter:
    """CPython pprint.PrettyPrinter 的最小子集。"""

    indent: int
    width: int
    depth: int

    def __init__(self, indent: int = 1, width: int = 80, depth: int = -1) -> None:
        self.indent = indent
        self.width = width
        self.depth = depth

    def pformat(self, obj: object) -> str:
        return pformat(obj, self.indent, self.width, self.depth)

    def pprint(self, obj: object) -> None:
        print(self.pformat(obj))
