"""pprint —— 结构化数据的可读打印（纯 pyadamas 源码，stdlib3）。

覆盖 pformat / pprint / saferepr / pp 与 PrettyPrinter 类；参数
indent / width / depth / sort_dicts 生效，compact 只做接口兼容。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- 包装策略是「塞得下就一行、塞不下在逗号处换行并按 indent 缩进」的贪心
  实现，不逐字复刻 CPython 的 _format 状态机；短对象输出与 CPython 一致；
- depth 用 -1 表示不限制（CPython 用 None，pa 的形参默认值只能是字面量）；
- stream 参数不做：pprint 一律写 stdout；
- bytes 在 pa 里与 str 共用运行期表示、unknown 实参上分不出来，按 str 渲染
  （CPython 输出 b'...'）；
- 循环引用不做检测（CPython 有 isrecursive 保护）。
"""


def _to_int(x: unknown) -> int:
    return x


def _repr_scalar(x: unknown) -> str:
    if x is None:
        return "None"
    if isinstance(x, bool):
        if x:
            return "True"
        return "False"
    if isinstance(x, str):
        return repr(x)
    if isinstance(x, int):
        return str(x)
    if isinstance(x, float):
        return repr(x)
    return str(x)


def _key_text(k: unknown) -> str:
    return str(k)


def _lookup(d: dict[unknown, unknown], k: unknown) -> unknown:
    return d[k]


def _all_ints(xs: list[unknown]) -> bool:
    for x in xs:
        if not isinstance(x, int):
            return False
    return True


def _int_list(xs: list[unknown]) -> list[int]:
    out: list[int] = []
    for x in xs:
        out.append(_to_int(x))
    return out


def _untyped(xs: list[int]) -> list[unknown]:
    out: list[unknown] = []
    for x in xs:
        out.append(x)
    return out


def _sorted_unknown(xs: list[unknown]) -> list[unknown]:
    """尽量按值的自然序排（全 int 时按数值，否则按文本），确定性输出。"""
    if len(xs) == 0:
        return []
    if _all_ints(xs):
        return _untyped(sorted(_int_list(xs)))
    idx: list[int] = []
    i = 0
    while i < len(xs):
        idx.append(i)
        i += 1

    def keyf(i: int) -> str:
        return _key_text(xs[i])

    idx = sorted(idx, key=keyf)
    out: list[unknown] = []
    for i in idx:
        out.append(xs[i])
    return out


def _wrap(open_s: str, parts: list[str], close_s: str, level: int,
          indent: int, width: int, cur: int) -> str:
    if len(parts) == 0:
        return open_s + close_s
    inner = indent * (level + 1)
    buf = open_s + parts[0]
    col = cur + len(open_s) + len(parts[0])
    i = 1
    while i < len(parts):
        p = parts[i]
        if col + 2 + len(p) > width:
            buf = buf + ",\n" + " " * inner + p
            col = inner + len(p)
        else:
            buf = buf + ", " + p
            col = col + 2 + len(p)
        i += 1
    return buf + close_s


def _fmt(obj: unknown, level: int, indent: int, width: int, depth: int,
         cur: int, sort_dicts: bool) -> str:
    if isinstance(obj, dict):
        return _fmt_dict(obj, level, indent, width, depth, cur, sort_dicts)
    if isinstance(obj, list):
        return _fmt_list(obj, level, indent, width, depth, cur, sort_dicts)
    if isinstance(obj, tuple):
        return _fmt_tuple(obj, level, indent, width, depth, cur, sort_dicts)
    if isinstance(obj, set):
        return _fmt_set(obj, level, indent, width, depth, cur, sort_dicts)
    return _repr_scalar(obj)


def _fmt_dict(d: dict[unknown, unknown], level: int, indent: int, width: int,
              depth: int, cur: int, sort_dicts: bool) -> str:
    if depth >= 0 and level >= depth:
        return "{...}"
    keys: list[unknown] = []
    for k in d:
        keys.append(k)
    if sort_dicts:
        keys = _sorted_unknown(keys)
    parts: list[str] = []
    for k in keys:
        key_s = _fmt(k, level + 1, indent, width, depth, cur, sort_dicts)
        val = _lookup(d, k)
        parts.append(key_s + ": " + _fmt(val, level + 1, indent, width, depth,
                                        cur, sort_dicts))
    return _wrap("{", parts, "}", level, indent, width, cur)


def _fmt_list(lst: list[unknown], level: int, indent: int, width: int,
              depth: int, cur: int, sort_dicts: bool) -> str:
    if depth >= 0 and level >= depth:
        return "[...]"
    parts: list[str] = []
    for v in lst:
        parts.append(_fmt(v, level + 1, indent, width, depth, cur, sort_dicts))
    return _wrap("[", parts, "]", level, indent, width, cur)


def _fmt_tuple(tup: tuple[unknown, ...], level: int, indent: int, width: int,
               depth: int, cur: int, sort_dicts: bool) -> str:
    if depth >= 0 and level >= depth:
        return "(...)"
    parts: list[str] = []
    for v in tup:
        parts.append(_fmt(v, level + 1, indent, width, depth, cur, sort_dicts))
    if len(parts) == 1:
        return "(" + parts[0] + ",)"
    return _wrap("(", parts, ")", level, indent, width, cur)


def _fmt_set(s: set, level: int, indent: int, width: int, depth: int,
             cur: int, sort_dicts: bool) -> str:
    if depth >= 0 and level >= depth:
        return "{...}"
    vals: list[unknown] = []
    for v in s:
        vals.append(v)
    vals = _sorted_unknown(vals)
    if len(vals) == 0:
        return "set()"
    parts: list[str] = []
    for v in vals:
        parts.append(_fmt(v, level + 1, indent, width, depth, cur, sort_dicts))
    return _wrap("{", parts, "}", level, indent, width, cur)


def saferepr(obj: unknown, indent: int = 1, width: int = 80,
             depth: int = -1, sort_dicts: bool = True) -> str:
    """repr 的安全版本（同 pformat 的标量口径）。"""
    return _fmt(obj, 0, indent, width, depth, 0, sort_dicts)


def pformat(obj: unknown, indent: int = 1, width: int = 80, depth: int = -1,
            compact: bool = False, sort_dicts: bool = True) -> str:
    """把对象格式化成可读文本。"""
    return _fmt(obj, 0, indent, width, depth, 0, sort_dicts)


def pprint(obj: unknown, indent: int = 1, width: int = 80, depth: int = -1,
           compact: bool = False, sort_dicts: bool = True) -> None:
    """把对象格式化后打印到 stdout。"""
    print(pformat(obj, indent, width, depth, compact, sort_dicts))


pp = pprint


class PrettyPrinter:
    """可复用的格式化器（参数缓存进实例）。"""

    indent: int
    width: int
    depth: int
    sort_dicts: bool

    def __init__(self, indent: int = 1, width: int = 80, depth: int = -1,
                 compact: bool = False, sort_dicts: bool = True) -> None:
        self.indent = indent
        self.width = width
        self.depth = depth
        self.sort_dicts = sort_dicts

    def pformat(self, obj: unknown) -> str:
        return pformat(obj, self.indent, self.width, self.depth, False,
                       self.sort_dicts)

    def pprint(self, obj: unknown) -> None:
        print(self.pformat(obj))
