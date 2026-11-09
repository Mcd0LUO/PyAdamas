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


def gen_ops(rng, n):
    """生成器：for / next / StopIteration / close 清理 / yield from。"""
    count = rng.randint(2, 6)
    step = rng.randint(1, 4)
    base = rng.randint(-9, 9)
    lines = [
        "def gen0(n: int):",
        "    i = 0",
        "    while i < n:",
        f"        yield {base} + i * {step}",
        "        i += 1",
        "",
        "def dgen(n: int):",
        "    yield from gen0(n)",
        "    yield from [n, n + 1]",
        "",
        "def cgen(n: int):",
        "    try:",
        "        yield from dgen(n)",
        "    finally:",
        "        print('dcleanup')",
        "",
    ]
    for _ in range(n):
        r = rng.random()
        if r < 0.4:
            lines.append(f"for v in gen0({count}):")
            lines.append("    print(v)")
        elif r < 0.6:
            lines.append(f"for v in dgen({count}):")
            lines.append("    print(v)")
        elif r < 0.7:
            lines.append(f"g = cgen({count})")
            lines.append("print(next(g))")
            lines.append("g.close()")
        elif r < 0.85:
            lines.append(f"g = gen0({count})")
            lines.append("try:")
            lines.append("    while True:")
            lines.append("        print(next(g))")
            lines.append("except StopIteration:")
            lines.append("    print('done')")
        else:
            lines.append(f"g = gen0({count})")
            lines.append("try:")
            lines.append("    print(next(g))")
            lines.append("finally:")
            lines.append("    print('cleanup')")
            lines.append("g.close()")
        lines.append("")
    return "\n".join(lines)


def async_ops(rng, n):
    """async/await：asyncio.run、sleep(0)、gather、异常跨 await。"""
    k = rng.randint(-9, 9)
    m = rng.randint(1, 4)
    k2 = rng.randint(-9, 9)
    k3 = rng.randint(-9, 9)
    lines = [
        "import asyncio",
        "",
        "async def f0(x: int) -> int:",
        "    await asyncio.sleep(0)",
        f"    return x + {k}",
        "",
        "async def f1(x: int) -> int:",
        "    a = await f0(x)",
        "    b = await f0(a)",
        f"    return a * {m} + b",
        "",
        "async def fail() -> int:",
        "    await asyncio.sleep(0.001)",
        "    raise ValueError('diff')",
        "",
        "async def agen(k: int):",
        "    i = 0",
        "    while i < k:",
        "        await asyncio.sleep(0)",
        "        yield i",
        "        i += 1",
        "",
        "class Ctx:",
        "    async def __aenter__(self) -> int:",
        "        await asyncio.sleep(0)",
        f"        return {k}",
        "",
        "    async def __aexit__(self, a, b, c) -> bool:",
        "        await asyncio.sleep(0)",
        "        return False",
        "",
        "async def catcher() -> int:",
        "    try:",
        "        await fail()",
        "    except ValueError:",
        f"        return {k}",
        "    return 0",
        "",
        "async def main() -> int:",
        f"    t = asyncio.create_task(f0({k2}))",
        f"    vals = await asyncio.gather(f1({k3}), t)",
        "    s = 0",
        "    for v in vals:",
        "        s += v",
        "    async for v in agen(3):",
        "        s += v",
        "    comp = [v * 2 async for v in agen(3)]",
        "    for v in comp:",
        "        s += v",
        "    evens = [v async for v in agen(4) if v % 2 == 0]",
        "    s += len(evens)",
        "    table = {v: v + 1 async for v in agen(2)}",
        "    s += table[1]",
        "    async with Ctx() as c:",
        "        s += c",
        "    return s",
        "",
        "async def victim() -> int:",
        "    try:",
        "        await asyncio.sleep(10)",
        "        return 1",
        "    except CancelledError:",
        "        return -5",
        "",
        "async def canceler() -> int:",
        "    t = asyncio.create_task(victim())",
        "    ok = t.cancel()",
        "    if ok:",
        "        print('req')",
        "    try:",
        "        v = await t",
        "        return v",
        "    except:",
        "        return -7",
        "",
    ]
    for _ in range(n):
        r = rng.random()
        if r < 0.4:
            lines.append("print(asyncio.run(main()))")
        elif r < 0.65:
            lines.append("print(asyncio.run(canceler()))")
        elif r < 0.85:
            lines.append("print(asyncio.run(catcher()))")
        else:
            lines.append(f"print(asyncio.run(f1({rng.randint(-9, 9)})))")
    return "\n".join(lines) + "\n"


GENERATORS = {
    "expr": (expr, 60),
    "struct": (struct, 40),
    "float": (float_ops, 60),
    "repr": (repr_doubles, 40),
    "gen": (gen_ops, 40),
    "async": (async_ops, 30),
}
