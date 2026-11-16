"""fnmatch 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

fnmatch 把 shell 通配符模式翻译成正则再匹配。依赖 re。
语义与 CPython 一致：`*` 匹配任意字符（含 `/`，与 glob 不同）、`?` 匹配单字符、
`[seq]` 匹配字符集、`[!seq]` 取反。
"""

import re


def translate(pat: str) -> str:
    """把 shell 通配符模式翻译成等价的正则表达式。"""
    res: str = ""
    i: int = 0
    n: int = len(pat)
    while i < n:
        c: str = pat[i]
        i += 1
        if c == "*":
            res = res + ".*"
        elif c == "?":
            res = res + "."
        elif c == "[":
            j: int = i
            if j < n and pat[j] == "!":
                j += 1
            if j < n and pat[j] == "]":
                j += 1
            while j < n and pat[j] != "]":
                j += 1
            if j >= n:
                res = res + "\\["
            else:
                stuff: str = pat[i:j]
                i = j + 1
                if stuff[:1] == "!":
                    stuff = "^" + stuff[1:]
                elif stuff[:1] == "^":
                    stuff = "\\" + stuff
                res = res + "[" + stuff + "]"
        else:
            res = res + re.escape(c)
    return "(?s:" + res + ")\\Z"


def fnmatch(name: str, pat: str) -> bool:
    """测试单个文件名是否匹配模式。"""
    p = re.compile(translate(pat))
    return p.match(name) is not None


def fnmatchcase(name: str, pat: str) -> bool:
    """区分大小写的匹配（本实现没有做大小写归一，所以与 fnmatch 相同）。"""
    return fnmatch(name, pat)


def filter(names: list, pat: str) -> list:
    """返回 names 里匹配 pat 的那些。"""
    out: list = []
    p = re.compile(translate(pat))
    i: int = 0
    while i < len(names):
        if p.match(names[i]) is not None:
            out.append(names[i])
        i += 1
    return out
