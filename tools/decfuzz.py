#!/usr/bin/env python3
"""decimal fuzz：pyadamas stdlib/decimal.py vs CPython 3.14 逐字节对拍。

pyadamas 的 float 构造口径是「最短 repr 再解析」（不是 CPython 的二进制
精确展开），所以黄金侧把 Decimal(float) 换成 _D(repr(float))；其余
（str/int/big/Decimal 构造、__format__ 全谱）保持同一份源码。

用法：python3 tools/decfuzz.py [seed] [随机说明符条数]
"""
import random
import subprocess
import sys

PA = "./build/pyadamas"
GEN = "/tmp/wt-dec2/tmp_fuzz.py"
GENC = "/tmp/wt-dec2/tmp_fuzz_cpy.py"

SHIM = """from decimal import Decimal as _D, getcontext  # noqa: F401
def Dec(v="0", ctx=None):
    if isinstance(v, float):
        return _D(repr(v))
    if isinstance(v, bool):
        return _D(int(v))
    return _D(v)
"""

SPEC_HEADS = ["", "+", " ", "-", "z", "#", "0", "+0", " 0", "z0", "#0", "+z0"]
FILLS = ["", "*", "0", ".", ",", "x", "=", "^", "5", "_"]
ALIGNS = ["", "<", ">", "=", "^"]
WIDTHS = ["", "0", "1", "2", "3", "5", "8", "10", "15", "24", "40"]
GROUPS = ["", ",", "_"]
PRECS = ["", ".", ".0", ".1", ".2", ".3", ".5", ".8", ".15", ".20", ".2,", "._", ".6_"]
# 'n' 有意拒绝（依赖 locale），不进对拍；单独在 409 里验 ValueError。
TYPES = ["", "e", "E", "f", "F", "g", "G", "%", "d", "s", "x", "!", "q"]

# 角矩阵：零（各种指数/符号）、一般值、极值
CORNER = ['"0"', '"-0"', '"0.00"', '"-0.00"', '"0E+3"', '"0E-5"',
          '"1.5"', '"-1.5"', '"9.995"', '"2.5"', '"1234.5678"',
          '"0.000123456"', '"1E+20"', '"1E-20"', '"12345678"',
          '"9999999999999999999999999999999999"', '"1.2300"']
CORNER_PRECS = ["", ".0", ".1", ".3", ".7"]
CORNER_HEADS = ["", "+", " ", "z", "#", ",", "_", "020", "^24", "=+24",
                "*<24", ",.2f", "_.2f", "020,.4f", "0=+30"]

VALUES = ['"0"', '"1"', '"-1"', '"1.5"', '"-1.5"', '"1234.5678"', '"-1234.5678"',
          '"0.000123456"', '"1E+20"', '"1E-20"', '"9.995"', '"2.5"', '"0.5"',
          '"100000000000000000000000000000"', '"0.00"', '"-0.00"', '"0E+3"',
          '"123456789012345678901234567890.12345"', '"1E+100"', '"1E-100"',
          '"9999999999999999999999999999999999"', '"1.2300"', '"7.0"']

FLOATS = ["0.1", "1.5", "-1.5", "0.0", "-0.0", "3.0", "1e16", "1e-7", "1e300",
          "5e-324", "2.5", "123456789.123456", "0.3333333333333333",
          "1.7976931348623157e308", "2.2250738585072014e-308", "1e22", "1e23"]


def esc(s):
    return s.replace('"', '\\"')


def fmt_line(out, vexpr, vlabel, spec):
    """发一条 format 调用；ValueError 与正常结果都打印定长标记。"""
    tag = "%s|%s|" % (vlabel.strip('"').replace("'", "").replace('"', ""),
                      spec.replace("'", "").replace('"', ""))
    out.append("try:")
    out.append('    print("%s" + repr(format(Decimal(%s), "%s")))'
               % (tag, vexpr, esc(spec)))
    out.append("except ValueError:")
    out.append('    print("%sVE")' % tag)


def gen_spec(rng):
    fill = rng.choice(FILLS) if rng.random() < 0.45 else ""
    align = rng.choice(ALIGNS) if rng.random() < 0.6 else ""
    if fill and not align:
        align = rng.choice(["<", ">", "=", "^"])
    return (fill + align + rng.choice(SPEC_HEADS) + rng.choice(WIDTHS)
            + rng.choice(GROUPS) + rng.choice(PRECS) + rng.choice(TYPES))


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    nlines = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    rng = random.Random(seed)
    out = ['print(getcontext().prec, getcontext().rounding)']
    # 1) 构造来源
    for lit in VALUES:
        out.append("t = Decimal(%s).as_tuple()" % lit)
        out.append('print(str(Decimal(%s)), repr(Decimal(%s)), t[0], t[1], t[2])'
                   % (lit, lit))
    for f in FLOATS:
        out.append("print(str(Decimal(%s)), repr(Decimal(%s)))" % (f, f))
    out.append("print(str(Decimal(0)), str(Decimal(7)), str(Decimal(-7)))")
    out.append("n: int = 1234567")
    out.append("print(str(Decimal(n)))")
    out.append("b: big = 123456789012345678901234567890123456789")
    out.append("print(str(Decimal(b)))")
    out.append('print(str(Decimal(Decimal("1.2300"))), str(Decimal(Decimal("-0.00"))))')
    out.append('print(str(Decimal("1.5", 0)), str(Decimal(1, None)))')
    # 2) 角矩阵：每个值跑全部类型 × 精度
    for v in CORNER:
        for typ in ["", "e", "E", "f", "F", "g", "G", "%"]:
            for pr in CORNER_PRECS:
                fmt_line(out, v, v, pr + typ)
    # 3) 角矩阵：修饰组合
    for v in CORNER[:8]:
        for head in CORNER_HEADS:
            fmt_line(out, v, v, head + "f")
            fmt_line(out, v, v, head)
    # 4) 随机说明符
    for _ in range(nlines):
        v = rng.choice(VALUES + FLOATS)
        fmt_line(out, v, v, gen_spec(rng))
    src = "\n".join(out) + "\n"
    with open(GEN, "w") as f:
        f.write("from decimal import Decimal, getcontext\n" + src)
    with open(GENC, "w") as f:
        f.write(SHIM + "from decimal import getcontext\n"
                + src.replace("Decimal(", "Dec("))
    pa = subprocess.run([PA, "run", GEN], capture_output=True, text=True)
    cp = subprocess.run([sys.executable, GENC], capture_output=True, text=True)
    a = pa.stdout.splitlines()
    b = cp.stdout.splitlines()
    bad = 0
    n = max(len(a), len(b))
    for i in range(n):
        x = a[i] if i < len(a) else "<missing>"
        y = b[i] if i < len(b) else "<missing>"
        if x != y:
            bad += 1
            if bad <= 15:
                print("LINE %d\n  pa : %r\n  cpy: %r" % (i + 1, x, y))
    if pa.returncode != 0:
        print("pyadamas exit", pa.returncode, pa.stderr[:2000])
    if cp.returncode != 0:
        print("cpython exit", cp.returncode, cp.stderr[:2000])
    print("seed=%d src_lines=%d compared=%d diff=%d" % (seed, len(out), n, bad))
    return 0 if bad == 0 and pa.returncode == 0 and cp.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
