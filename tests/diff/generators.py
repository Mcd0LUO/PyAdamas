"""差分测试的程序生成器。

每个生成器接收 random.Random 和行数，返回一段 py23 源码。硬约束是：生成的
程序在 CPython 和 py23 下必须输出一致。所以刻意避开已知的语义分歧（整数
溢出、列表相等、移位位数、读未初始化的局部变量），那些分歧写在
docs/compatibility.md 里，不在这里测。

本文件是「py23 能跑的子集」：不 import 第三方模块、不用函数值容器，
random.Random 参数用函数体内的 `random.Random` 标注收窄（py23 的函数签名
注解在 import 处理之前解析，模块属性注解只能写在函数体里）。

新增生成器时注意两点：
  - 只生成两端都支持的语法（`check` 过、`build` 也过）；
  - 用 `random.Random` 实例，不要碰全局随机状态。
"""
import random


def _int_expr(rng, depth: int, names: list[str]) -> str:
    r: random.Random = rng
    if depth > 2 or r.random() < 0.35:
        if len(names) > 0 and r.random() < 0.5:
            return r.choice(names)
        return str(r.randint(-50, 50))
    op = r.choice(["+", "-", "*", "//", "%", "&", "|", "^", "<<", ">>"])
    a = _int_expr(r, depth + 1, names)
    b = _int_expr(r, depth + 1, names)
    if op == "//" or op == "%":
        b = f"(({b}) | 1)"  # 保证非零
    if op == "<<" or op == ">>":
        b = str(r.randint(0, 5))  # 移位超 63 位在 py23 里按 64 取模
    return f"({a} {op} {b})"


def _float_expr(rng, depth: int) -> str:
    r: random.Random = rng
    if depth > 2 or r.random() < 0.4:
        return f"{r.uniform(-12.0, 12.0):.4f}"
    op = r.choice(["+", "-", "*", "//", "%"])
    a = _float_expr(r, depth + 1)
    b = _float_expr(r, depth + 1)
    if op == "//" or op == "%":
        b = f"({b} + 13.0)"  # 远离 0，避免除零和溢出
    return f"({a} {op} {b})"


def expr(rng, n: int) -> str:
    """纯表达式：整数与浮点的四则、位运算、整除取模。"""
    r: random.Random = rng
    lines: list[str] = []
    i = 0
    while i < n:
        if r.random() < 0.6:
            lines.append(f"print({_int_expr(r, 0, [])})")
        else:
            lines.append(f"print({_float_expr(r, 0)})")
        i += 1
    return "\n".join(lines) + "\n"


def struct(rng, n: int) -> str:
    """带函数、循环、分支的结构化程序。"""
    r: random.Random = rng
    lines: list[str] = [
        "def f0(a: int, b: int) -> int:",
        f"    return {_int_expr(r, 0, ['a', 'b'])}",
        "",
        "def main() -> None:",
    ]
    names: list[str] = []
    i = 0
    while i < n:
        pick = r.random()
        if pick < 0.25:
            v = f"v{i}"
            lines.append(f"    {v} = {_int_expr(r, 0, names)}")
            names.append(v)
        elif pick < 0.45:
            v = f"v{i}"
            lines.append(f"    for {v} in range({r.randint(1, 5)}):")
            lines.append(f"        print({_int_expr(r, 0, names)})")
            names.append(v)
        elif pick < 0.6:
            lines.append(f"    if {_int_expr(r, 0, names)} > 0:")
            lines.append(f"        print('pos', {_int_expr(r, 0, names)})")
            lines.append("    else:")
            lines.append(f"        print('nonpos', {_int_expr(r, 0, names)})")
        elif pick < 0.75:
            lines.append(f"    print(f0({_int_expr(r, 0, names)}, "
                         f"{_int_expr(r, 0, names)}))")
        else:
            e = _int_expr(r, 0, names)
            lines.append(f"    print({e})")
            lines.append(f"    if {e} % 7 == 0:")
            lines.append("        pass")
        i += 1
    lines.append("")
    lines.append("main()")
    return "\n".join(lines) + "\n"


_SEPS = ['" "', '""', '"|"', '"-"', '"\\t"']
_ENDS = ['"\\n"', '""', '"\\n\\n"', '" "', '"<E>"']


def float_ops(rng, n: int) -> str:
    """浮点的 // 与 %（符号修正）、print 的 sep/end。"""
    r: random.Random = rng
    lines: list[str] = []
    i = 0
    while i < n:
        pick = r.random()
        if pick < 0.45:
            lines.append(f"print({_float_expr(r, 0)})")
        elif pick < 0.7:
            lines.append(f"print({_int_expr(r, 0, [])})")
        elif pick < 0.9:
            args = ""
            cnt = r.randint(0, 4)
            j = 0
            while j < cnt:
                if j > 0:
                    args += ", "
                if r.random() < 0.5:
                    args += _float_expr(r, 0)
                else:
                    args += _int_expr(r, 0, [])
                j += 1
            kw = ""
            if r.random() < 0.7:
                kw = "sep=" + r.choice(_SEPS)
            if r.random() < 0.7:
                if kw != "":
                    kw += ", "
                kw += "end=" + r.choice(_ENDS)
            sep = ""
            if args != "" and kw != "":
                sep = ", "
            lines.append(f"print({args}{sep}{kw})")
        else:
            lines.append(f"print(f'{{{_float_expr(r, 0)}}} ok')")
        i += 1
    return "\n".join(lines) + "\n"


# 覆盖浮点转字符串的常见边角：0/-0、次正规、最大最小正规值、精确整数边界。
_REPR_LITERALS = [
    "0.0",
    "-0.0",
    "1.0",
    "-1.0",
    "0.1",
    "1.5",
    "3.141592653589793",
    "1e-300",
    "1e300",
    "5e-324",
    "2.2250738585072014e-308",
    "1.7976931348623157e308",
    "9007199254740992.0",
    "123456789.123456789",
    "1e16",
    "1e-16",
]


def repr_doubles(rng, n: int) -> str:
    """有限 double 的逐行 print，专测浮点转字符串。

    py23 没有 struct/bit 重解释，用固定边角值 + 十进制定标随机值代替。
    """
    r: random.Random = rng
    out: list[str] = []
    while len(out) < n:
        if r.random() < 0.5:
            out.append(f"print({r.choice(_REPR_LITERALS)})")
            continue
        mant = r.uniform(-1.0, 1.0)
        e = r.randint(0, 300)
        scale = 1.0
        j = 0
        while j < e:
            scale *= 10.0
            j += 1
        v = 0.0
        if r.random() < 0.5:
            v = mant * scale
        else:
            v = mant / scale
        if v != 0.0 and v != v:
            continue  # 理论上不会出现；防守一下
        out.append(f"print({v!r})")
    return "\n".join(out) + "\n"


def gen_ops(rng, n: int) -> str:
    """生成器：for / next / StopIteration / close 清理 / yield from。"""
    r: random.Random = rng
    count = r.randint(2, 6)
    step = r.randint(1, 4)
    base = r.randint(-9, 9)
    lines: list[str] = [
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
    i = 0
    while i < n:
        pick = r.random()
        if pick < 0.4:
            lines.append(f"for v in gen0({count}):")
            lines.append("    print(v)")
        elif pick < 0.6:
            lines.append(f"for v in dgen({count}):")
            lines.append("    print(v)")
        elif pick < 0.7:
            lines.append(f"g = cgen({count})")
            lines.append("print(next(g))")
            lines.append("g.close()")
        elif pick < 0.85:
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
        i += 1
    return "\n".join(lines)


def async_ops(rng, n: int) -> str:
    """async/await：asyncio.run、sleep(0)、gather、异常跨 await。"""
    r: random.Random = rng
    k = r.randint(-9, 9)
    m = r.randint(1, 4)
    k2 = r.randint(-9, 9)
    k3 = r.randint(-9, 9)
    lines: list[str] = [
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
    i = 0
    while i < n:
        pick = r.random()
        if pick < 0.4:
            lines.append("print(asyncio.run(main()))")
        elif pick < 0.65:
            lines.append("print(asyncio.run(canceler()))")
        elif pick < 0.85:
            lines.append("print(asyncio.run(catcher()))")
        else:
            lines.append(f"print(asyncio.run(f1({r.randint(-9, 9)})))")
        i += 1
    return "\n".join(lines) + "\n"


GENERATOR_NAMES = ["expr", "struct", "float", "repr", "gen", "async"]


def default_cases(name: str) -> int:
    if name == "expr":
        return 60
    if name == "struct":
        return 40
    if name == "float":
        return 60
    if name == "repr":
        return 40
    if name == "gen":
        return 40
    if name == "async":
        return 30
    return 0
