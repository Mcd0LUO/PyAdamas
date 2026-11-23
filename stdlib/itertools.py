"""itertools 的 stdlib 扩展层（原生模块 + Python 补丁）。

原生实现提供 chain / islice / product / permutations / combinations /
combinations_with_replacement / zip_longest / accumulate / repeat 等需要
底层循环或状态机的部分；本文件补的是「用列表几行就能写完」的算子。

starmap(f, pairs) 用「arity 分派」实现：本项目不能对 unknown 可调用对象
做 `f(*pairs[i])`，但可以在小元数（1~4）上按 `len(row)` 分支显式传参；
超过 4 个实参的行会报 ValueError。tee(it, n) 返回 n 个 list 副本（没有惰性
迭代器）。chain.from_iterable 写成 `itertools.chain.from_iterable(...)` 会
触发「func 对象没有方法」的编译错误（PA 把 chain 当函数而不是类），因此
提供 `chain_iterables(lists)` 代替，语义与 CPython 的 chain.from_iterable
等价（展平一层）。

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

def starmap(f, pairs: list) -> list:
    """把 pairs 里每个子列表/元组展开成实参调用 f，返回 list。

    本项目不能对 unknown 可调用对象做 f(*row)，所以按子行的
    len 做 arity 分派（1~4 个实参）。超过 4 个实参的行抛
    ValueError。与 CPython itertools.starmap 的结果集合一致，
    但这里是立即求值、返回 list，不是惰性迭代器。
    """
    out: list = []
    i: int = 0
    while i < len(pairs):
        row = pairs[i]
        n: int = len(row)
        if n == 1:
            out.append(f(row[0]))
        elif n == 2:
            out.append(f(row[0], row[1]))
        elif n == 3:
            out.append(f(row[0], row[1], row[2]))
        elif n == 4:
            out.append(f(row[0], row[1], row[2], row[3]))
        else:
            raise ValueError("starmap: row has more than 4 items")
        i += 1
    return out


def tee(it: list, n: int = 2) -> list:
    """把 it 复制 n 份独立的 list，返回「list 的 list」。

    CPython 的 tee 返回 n 个惰性迭代器且共享底层缓存；本项目没有
    惰性迭代器，因此这里返回 n 份立即复制的 list。每份都是独立
    对象，修改其中一份不影响其它份。这比 CPython 更占内存，但
    消费序列时结果一致。
    """
    out: list = []
    i: int = 0
    while i < n:
        c: list = []
        j: int = 0
        while j < len(it):
            c.append(it[j])
            j += 1
        out.append(c)
        i += 1
    return out


def chain_iterables(lists: list) -> list:
    """展平一层：等价于 CPython 的 itertools.chain.from_iterable(lists)。

    为什么不叫 chain.from_iterable：PA 把原生 chain 当函数对象，
    itertools.chain.from_iterable(...) 的「函数对象属性」写法会直接
    编译报错（'func' object has no method 'from_iterable'），不能给
    函数挂属性。故提供顶层函数 chain_iterables，调用方改用
    itertools.chain_iterables([[1],[2,3]]) 即可；结果一致（立即
    求值、返回 list）。
    """
    out: list = []
    i: int = 0
    while i < len(lists):
        sub = lists[i]
        j: int = 0
        while j < len(sub):
            out.append(sub[j])
            j += 1
        i += 1
    return out
