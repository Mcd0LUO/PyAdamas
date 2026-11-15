"""glob 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 glob / iglob / escape：支持 `*`、`?`、字符类 `[abc]`/`[a-z]`/`[!abc]`，
`glob(..., recursive=True)` 时 `**` 递归匹配 0 层或多层目录。结果是排序后的
list（iglob 也返回 list，没有惰性迭代器）。

语义边界：隐藏文件规则与 CPython 一致（通配符不以 . 开头时不匹配 . 开头
的名字）；不做大小写折叠；Unicode 按字节匹配。
"""

import os
from os.path import exists, isdir, join


def _has_magic(pat: str) -> bool:
    i = 0
    while i < len(pat):
        c = pat[i]
        if c == "*" or c == "?" or c == "[":
            return True
        i += 1
    return False


def _in_class(c: str, pat: str, begin: int, end: int) -> bool:
    """c 是否落在 pat[begin:end] 描述的字符类里（end 是 ] 的下标）。"""
    i = begin
    while i < end:
        if i + 2 < end and pat[i + 1] == "-":
            if pat[i] <= c and c <= pat[i + 2]:
                return True
            i += 3
        else:
            if pat[i] == c:
                return True
            i += 1
    return False


def _match(name: str, pat: str) -> bool:
    """单段通配匹配（等价于 fnmatch.fnmatchcase）。"""
    ni = 0
    pi = 0
    star = -1
    mark = 0
    while ni < len(name):
        if pi < len(pat) and (pat[pi] == name[ni] or pat[pi] == "?"):
            pi += 1
            ni += 1
        elif pi < len(pat) and pat[pi] == "*":
            star = pi
            mark = ni
            pi += 1
        elif pi < len(pat) and pat[pi] == "[":
            close = pi + 1
            if close < len(pat) and (pat[close] == "!" or pat[close] == "^"):
                close += 1
            while close < len(pat) and pat[close] != "]":
                close += 1
            if close >= len(pat):
                # 未闭合的 [：退化成字面字符
                if name[ni] == "[":
                    pi += 1
                    ni += 1
                elif star >= 0:
                    pi = star + 1
                    mark += 1
                    ni = mark
                else:
                    return False
            else:
                negate = pat[pi + 1] == "!" or pat[pi + 1] == "^"
                start = pi + 1
                if negate:
                    start += 1
                hit = _in_class(name[ni], pat, start, close)
                if hit != negate:
                    ni += 1
                    pi = close + 1
                elif star >= 0:
                    pi = star + 1
                    mark += 1
                    ni = mark
                else:
                    return False
        elif star >= 0:
            pi = star + 1
            mark += 1
            ni = mark
        else:
            return False
    while pi < len(pat) and pat[pi] == "*":
        pi += 1
    return pi == len(pat)


def _split(pattern: str) -> list[str]:
    raw = pattern.split("/")
    parts: list[str] = []
    for p in raw:
        if len(p) > 0:
            parts.append(p)
    return parts


def _add_all(base: str, out: list[str]) -> None:
    """** 递归：base 下所有文件与目录（含更深层）。"""
    if not isdir(base):
        return
    names = sorted(os.listdir(base))
    for name in names:
        sub = join(base, name)
        out.append(sub)
        if isdir(sub):
            _add_all(sub, out)


def _collect(base: str, parts: list[str], idx: int, recursive: bool,
             out: list[str]) -> None:
    part = parts[idx]
    last = idx + 1 >= len(parts)
    if recursive and part == "**":
        if last:
            out.append(base)
            _add_all(base, out)
            return
        _collect(base, parts, idx + 1, recursive, out)
        if not isdir(base):
            return
        names = sorted(os.listdir(base))
        for name in names:
            sub = join(base, name)
            if isdir(sub):
                _collect(sub, parts, idx, recursive, out)
        return
    if _has_magic(part):
        if not isdir(base):
            return
        names = sorted(os.listdir(base))
        allow_hidden = len(part) > 0 and part[0] == "."
        for name in names:
            if len(name) > 0 and name[0] == "." and not allow_hidden:
                continue
            if _match(name, part):
                full = join(base, name)
                if last:
                    out.append(full)
                elif isdir(full):
                    _collect(full, parts, idx + 1, recursive, out)
        return
    full = join(base, part)
    if last:
        if exists(full):
            out.append(full)
    elif isdir(full):
        _collect(full, parts, idx + 1, recursive, out)


def glob(pattern: str, recursive: bool = False) -> list[str]:
    """展开通配模式，返回排序后的路径 list。"""
    if len(pattern) == 0:
        return []
    absolute = pattern[0] == "/"
    parts = _split(pattern)
    if len(parts) == 0:
        if absolute:
            return ["/"]
        return []
    base = "/" if absolute else "."
    out: list[str] = []
    _collect(base, parts, 0, recursive, out)
    result = sorted(out)
    if absolute:
        return result
    cleaned: list[str] = []
    for p in result:
        if p.startswith("./"):
            cleaned.append(p[2:])
        else:
            cleaned.append(p)
    return cleaned


def iglob(pattern: str, recursive: bool = False) -> list[str]:
    """pyadamas 里没有惰性迭代器：与 glob 相同，返回 list。"""
    return glob(pattern, recursive)


def escape(pathname: str) -> str:
    """把 * ? [ 转义成字符类，让它们按字面匹配。"""
    out = ""
    i = 0
    while i < len(pathname):
        c = pathname[i]
        if c == "*" or c == "?" or c == "[":
            out = out + "[" + c + "]"
        else:
            out = out + c
        i += 1
    return out
