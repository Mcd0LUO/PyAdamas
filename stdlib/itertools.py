"""itertools 的 stdlib 扩展层（原生模块 + Python 补丁）。

原生实现提供 chain / islice / product / permutations / combinations /
combinations_with_replacement / zip_longest / accumulate / repeat 等需要
底层循环或状态机的部分；本文件补的是「用列表几行就能写完」的算子。

未提供 starmap：它需要把 tuple 展开成实参（`f(*args)`），而本项目暂不支持
`*` 展开 unknown 可调用对象。

语义边界：PyAdamas 没有通用迭代协议 —— 形参不能标注成「任意可迭代」。
所以这里的算子统一接收 list（调用方传 list 即可），返回 list 而不是惰性
迭代器。这与 CPython 的惰性语义不同，但结果集合一致；需要惰性时用原生
实现里的那几个。

注意：原生名字优先，本文件只新增原生没有的接口。
"""


def takewhile(predicate, xs: list) -> list:
    out: list = []
    i: int = 0
    while i < len(xs):
        x = xs[i]
        if not predicate(x):
            break
        out.append(x)
        i += 1
    return out


def dropwhile(predicate, xs: list) -> list:
    out: list = []
    i: int = 0
    while i < len(xs):
        if not predicate(xs[i]):
            break
        i += 1
    while i < len(xs):
        out.append(xs[i])
        i += 1
    return out


def filterfalse(predicate, xs: list) -> list:
    out: list = []
    i: int = 0
    while i < len(xs):
        if not predicate(xs[i]):
            out.append(xs[i])
        i += 1
    return out


def groupby(xs: list) -> list:
    out: list = []
    cur_key = None
    cur_items: list = []
    i: int = 0
    while i < len(xs):
        x = xs[i]
        if i == 0:
            cur_key = x
            cur_items = [x]
        elif x == cur_key:
            cur_items.append(x)
        else:
            out.append((cur_key, cur_items))
            cur_key = x
            cur_items = [x]
        i += 1
    if len(xs) > 0:
        out.append((cur_key, cur_items))
    return out


def pairwise(xs: list) -> list:
    out: list = []
    i: int = 1
    while i < len(xs):
        out.append((xs[i - 1], xs[i]))
        i += 1
    return out


def compress(data: list, selectors: list) -> list:
    out: list = []
    i: int = 0
    n: int = len(data)
    if len(selectors) < n:
        n = len(selectors)
    while i < n:
        if selectors[i]:
            out.append(data[i])
        i += 1
    return out
