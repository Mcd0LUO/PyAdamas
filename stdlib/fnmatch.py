"""fnmatch 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

fnmatch 把 shell 通配符模式翻译成正则再匹配。依赖 re。
语义与 CPython 一致：`*` 匹配任意字符（含 `/`，与 glob 不同）、`?` 匹配单字符、
`[seq]` 匹配字符集、`[!seq]` 取反。

与 CPython 的两处口径差（0.488）：
- translate() 的返回值是 "(?s:...)\Z"，起点锚由 fnmatch/filter 在 compile 时补 ^
  （pa 的 re 引擎里 ^ 是起点锚，\Z / \z / \A 都**不是**锚点，见 docs/stdlib-round2.md 7.1）。
  所以拿 translate() 的返回值自己再 re.match 只能得到「前缀命中」；要整串匹配请用
  fnmatch / fnmatchcase（内部用 ^ + search + end() 补齐两端）。
- 正则**文本**与 CPython 不逐字节一致（CPython 3.14 用原子组 (?>.*?b) 与 \z）；
  但**匹配结果**逐字节一致（7082 个模式的翻译等价性 + 14400 组匹配矩阵差分，0 差异）。
"""

import re

# re.escape 对 ASCII 的转义集合（CPython 3.7+ 与 pa 的 re.escape 实测一致：
# 9-13、32、35 36 38 40-43 45 46 63 91-94 123-126）。逐字符调用 re.escape
# 是原生调用，translate 里每个字面字符一次；这里用 128 项查表替代，
# 非 ASCII 仍回落到 re.escape（两边都不转义，回落只为口径保险）。
_SPECIAL: list[int] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0,
]


def _escape_char(c: str) -> str:
    """等价于 re.escape(c)（单字符），只对 ASCII 走查表快路径。"""
    o: int = ord(c)
    if o < 128:
        if _SPECIAL[o] != 0:
            return "\\" + c
        return c
    return re.escape(c)


def translate(pat: str) -> str:
    """把 shell 通配符模式翻译成等价的正则表达式。

    缓冲用 list[str] + join；此前是逐片段 `res = res + ...`，n 个字符的
    模式会产生 O(n) 个中间字符串（二次曲线）。
    """
    parts: list[str] = []
    i: int = 0
    n: int = len(pat)
    while i < n:
        c: str = pat[i]
        i += 1
        if c == "*":
            parts.append(".*")
        elif c == "?":
            parts.append(".")
        elif c == "[":
            j: int = i
            if j < n and pat[j] == "!":
                j += 1
            if j < n and pat[j] == "]":
                j += 1
            while j < n and pat[j] != "]":
                j += 1
            if j >= n:
                parts.append("\\[")
            else:
                stuff: str = pat[i:j]
                i = j + 1
                if stuff[:1] == "!":
                    stuff = "^" + stuff[1:]
                elif stuff[:1] == "^":
                    stuff = "\\" + stuff
                parts.append("[" + stuff + "]")
        else:
            parts.append(_escape_char(c))
    return "(?s:" + "".join(parts) + ")\\Z"


def _compile(pat: str):
    """编译模式，起点前置 ^。

    pa 的 re 引擎缺口（docs/stdlib-round2.md 7.1）：Pattern.match 与 search 同义
    （不锚定起点），\Z / \z / \A 都不被当作锚点，只有 ^ 与 $ 有效。所以 ^ 加在
    compile 这一侧（translate() 的输出保持原样，不做隐式加锚），终点由 _fullmatch
    用 m.end() 显式补齐。等价于 CPython 的 p.match(name) + \Z。
    """
    return re.compile("^" + translate(pat))


def _fullmatch(p, name: str) -> bool:
    """整串匹配：起点已由 ^ 锚住，这里只需补终点（失败路径在首字符就返回）。"""
    m = p.search(name)
    if m is None:
        return False
    return m.end() == len(name)


def fnmatch(name: str, pat: str) -> bool:
    """测试单个文件名是否匹配模式。"""
    return _fullmatch(_compile(pat), name)


def fnmatchcase(name: str, pat: str) -> bool:
    """区分大小写的匹配（本实现没有做大小写归一，所以与 fnmatch 相同）。"""
    return fnmatch(name, pat)


def filter(names: list, pat: str) -> list:
    """返回 names 里匹配 pat 的那些。"""
    out: list = []
    p = _compile(pat)
    i: int = 0
    n: int = len(names)
    while i < n:
        if _fullmatch(p, names[i]):
            out.append(names[i])
        i += 1
    return out
