"""差分测试的程序生成器。

每个生成器接收 random.Random，返回一段 py23 源码。硬约束是：生成的程序在
CPython 和 py23 下必须输出一致。所以刻意避开已知的语义分歧（整数溢出、
列表相等、移位位数、读未初始化的局部变量），那些分歧写在 docs/compatibility.md 里，
不在这里测。

新增生成器时注意两点：
  - 只生成两端都支持的语法（`check` 过、`build` 也过）；
  - 用 `random.Random` 实例，不要碰全局的 random 状态。
"""


def _int_expr(rng, depth=0, names=()):
    if depth > 2 or rng.random() < 0.35:
        return rng.choice(names + (str(rng.randint(-50, 50)),))
    op = rng.choice(["+", "-", "*", "//", "%", "&", "|", "^", "<<", ">>"])
    a = _int_expr(rng, depth + 1, names)
    b = _int_expr(rng, depth + 1, names)
    if op in ("//", "%"):
        b = f"(({b}) | 1)"  # 保证非零
    if op in ("<<", ">>"):
        b = str(rng.randint(0, 5))  # 移位超 63 位在 py23 里按 64 取模
    return f"({a} {op} {b})"


def _float_expr(rng, depth=0):
    if depth > 2 or rng.random() < 0.4:
        return f"{rng.uniform(-12, 12):.4f}"
    op = rng.choice(["+", "-", "*", "//", "%"])
    a = _float_expr(rng, depth + 1)
    b = _float_expr(rng, depth + 1)
    if op in ("//", "%"):
        b = f"({b} + 13.0)"  # 远离 0，避免除零和溢出
    return f"({a} {op} {b})"


def expr(rng, n):
    """纯表达式：整数与浮点的四则、位运算、整除取模。"""
    lines = []
    for _ in range(n):
        if rng.random() < 0.6:
            lines.append(f"print({_int_expr(rng)})")
        else:
            lines.append(f"print({_float_expr(rng)})")
    return "\n".join(lines) + "\n"


def struct(rng, n):
    """带函数、循环、分支的结构化程序。"""
    lines = [
        "def f0(a: int, b: int) -> int:",
        f"    return {_int_expr(rng, 0, ('a', 'b'))}",
        "",
        "def main() -> None:",
    ]
    names = []
    for i in range(n):
        r = rng.random()
        if r < 0.25:
            v = f"v{i}"
            lines.append(f"    {v} = {_int_expr(rng, 0, tuple(names))}")
            names.append(v)
        elif r < 0.45:
            v = f"v{i}"
            lines.append(f"    for {v} in range({rng.randint(1, 5)}):")
            lines.append(f"        print({_int_expr(rng, 0, tuple(names))})")
            names.append(v)
        elif r < 0.6:
            lines.append(f"    if {_int_expr(rng, 0, tuple(names))} > 0:")
            lines.append(f"        print('pos', {_int_expr(rng, 0, tuple(names))})")
            lines.append("    else:")
            lines.append(f"        print('nonpos', {_int_expr(rng, 0, tuple(names))})")
        elif r < 0.75:
            lines.append(f"    print(f0({_int_expr(rng, 0, tuple(names))}, "
                         f"{_int_expr(rng, 0, tuple(names))}))")
        else:
            e = _int_expr(rng, 0, tuple(names))
            lines.append(f"    print({e})")
            lines.append(f"    if {e} % 7 == 0:")
            lines.append("        pass")
    lines += ["", "main()"]
    return "\n".join(lines) + "\n"


_SEPS = ['" "', '""', '"|"', '"-"', '"\\t"']
_ENDS = ['"\\n"', '""', '"\\n\\n"', '" "', '"<E>"']


def float_ops(rng, n):
    """浮点的 // 与 %（符号修正）、print 的 sep/end。"""
    lines = []
    for _ in range(n):
        r = rng.random()
        if r < 0.45:
            lines.append(f"print({_float_expr(rng)})")
        elif r < 0.7:
            lines.append(f"print({_int_expr(rng)})")
        elif r < 0.9:
            args = ", ".join(_float_expr(rng) if rng.random() < 0.5 else _int_expr(rng)
                             for _ in range(rng.randint(0, 4)))
            kw = []
            if rng.random() < 0.7:
                kw.append("sep=" + rng.choice(_SEPS))
            if rng.random() < 0.7:
                kw.append("end=" + rng.choice(_ENDS))
            sep = ", " if args and kw else ""
            lines.append(f"print({args}{sep}{', '.join(kw)})")
        else:
            lines.append(f"print(f'{{{_float_expr(rng)}}} ok')")
    return "\n".join(lines) + "\n"


def repr_doubles(rng, n):
    """随机位模式的 double，逐行 print，专测浮点转字符串。"""
    import struct

    out = []
    while len(out) < n:
        bits = rng.getrandbits(64)
        v = struct.unpack("<d", struct.pack("<Q", bits))[0]
        if v != v or v in (float("inf"), float("-inf")):
            continue  # nan/inf 在两边都是 nan/inf，但这里只测有限值
        out.append(f"print({v!r})")
    return "\n".join(out) + "\n"


GENERATORS = {
    "expr": (expr, 60),
    "struct": (struct, 40),
    "float": (float_ops, 60),
    "repr": (repr_doubles, 40),
}
