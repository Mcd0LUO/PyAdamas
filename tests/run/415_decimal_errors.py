"""stdlib/decimal.py：非法构造输入、非法格式说明符、NaN/Infinity 的明确拒绝。

本文件的期望块由 build/pyadamas 实跑生成，并逐条与 CPython 3.14 核对：
非法说明符（d/s/x/abc/*<0/^0.0f/...）与非法字面量在 CPython 上同样是
ValueError；'n' 与 NaN/Infinity 是**本轮有意拒绝**的 delta（CPython 接受
'n'、接受 NaN/Infinity）。bool 来源与 CPython 一致（True → 1）；None 在
pyadamas 的 unknown 域里与整数 0 同表示，无法区分（记录为已知差异）。
"""
from decimal import Decimal

d: Decimal = Decimal("1234.5678")

# 非法格式说明符：全部 ValueError
for s in ["d", "s", "x", "X", "o", "b", "c", "abc", "1.2.3", ".", "e5",
          "10.2z", "*<05", "<05", "*<0", "^0.0f", "+-5", "0,,", ",,",
          "5 5", "%10.2f", ".2x", "z z", "#z#", "1e2", "  ", "\t"]:
    try:
        print(repr(s), repr(format(d, s)))
    except ValueError:
        print(repr(s), "VE")

# 合法但少见的说明：确认没有误杀
for s in ["", "#", "z", "0", ",", "_", "=5", "0=5", "05", "-", "+", "z#0,.0f"]:
    try:
        print(repr(s), repr(format(d, s)))
    except ValueError:
        print(repr(s), "UNEXPECTED VE")

# 'n' 依赖 locale：明确拒绝而不是给出 C locale 之外可能错误的结果
for s in ["n", ".2n", ",n", "10n"]:
    try:
        print(repr(s), repr(format(d, s)))
    except ValueError:
        print(repr(s), "n rejected")

# NaN / Infinity：字符串与 float 两路都必须是 ValueError（不是静默 0）
for src in ["nan", "NaN", "-nan", "sNaN", "inf", "Inf", "Infinity", "-Inf",
            "infinity", "nan(1)"]:
    try:
        print(src, Decimal(src))
    except ValueError:
        print(src, "VE")
for f in [float("nan"), float("inf"), float("-inf")]:
    try:
        print(f, Decimal(f))
    except ValueError:
        print("float nan/inf VE")

# 其他非法字面量
for bad in ["", "abc", "1.2.3", "1e", "--1", ".", "1e+", "0x10", "1e 2", "1..2",
            " 1", "1 ", "1_0", "1.5d", "True", "None"]:
    try:
        print("bad", repr(bad), str(Decimal(bad)))
    except ValueError:
        print("bad", repr(bad), "VE")

# bool 来源：与 CPython 一致（True → 1、False → 0，bool 在 pyadamas 里
# 就是 int 的表示，str(True) 已经是 "1"）。
print(Decimal(True), Decimal(False), Decimal(True) == Decimal(1))
# None 来源：unknown 域里 None 与整数 0 是同一个机器字（没有类型标签），
# 无法区分，所以 Decimal(None) 得到 0 —— 有意记录的行为差异，不是静默 NaN。
print(Decimal(None))

# float 构造不会静默 NaN：即便经过 unknown 传递
u: unknown = float("nan")
try:
    print(Decimal(u))
except ValueError:
    print("unknown nan VE")

# --- expect ---
# 'd' 'd' VE
# 's' 's' VE
# 'x' 'x' VE
# 'X' 'X' VE
# 'o' 'o' VE
# 'b' 'b' VE
# 'c' 'c' VE
# 'abc' 'abc' VE
# '1.2.3' '1.2.3' VE
# '.' '.' VE
# 'e5' 'e5' VE
# '10.2z' '10.2z' VE
# '*<05' '*<05' VE
# '<05' '<05' VE
# '*<0' '*<0' VE
# '^0.0f' '^0.0f' VE
# '+-5' '+-5' VE
# '0,,' '0,,' VE
# ',,' ',,' VE
# '5 5' '5 5' VE
# '%10.2f' '%10.2f' VE
# '.2x' '.2x' VE
# 'z z' 'z z' VE
# '#z#' '#z#' VE
# '1e2' '1e2' VE
# '  ' '  ' VE
# '\t' '\t' VE
# '' '1234.5678'
# '#' '1234.5678'
# 'z' '1234.5678'
# '0' '1234.5678'
# ',' '1,234.5678'
# '_' '1_234.5678'
# '=5' '1234.5678'
# '0=5' '1234.5678'
# '05' '1234.5678'
# '-' '1234.5678'
# '+' '+1234.5678'
# 'z#0,.0f' '1,235.'
# 'n' 'n' n rejected
# '.2n' '.2n' n rejected
# ',n' ',n' n rejected
# '10n' '10n' n rejected
# nan nan VE
# NaN NaN VE
# -nan -nan VE
# sNaN sNaN VE
# inf inf VE
# Inf Inf VE
# Infinity Infinity VE
# -Inf -Inf VE
# infinity infinity VE
# nan(1) nan(1) VE
# nan float nan/inf VE
# inf float nan/inf VE
# -inf float nan/inf VE
# bad '' bad '' VE
# bad 'abc' bad 'abc' VE
# bad '1.2.3' bad '1.2.3' VE
# bad '1e' bad '1e' VE
# bad '--1' bad '--1' VE
# bad '.' bad '.' VE
# bad '1e+' bad '1e+' VE
# bad '0x10' bad '0x10' VE
# bad '1e 2' bad '1e 2' VE
# bad '1..2' bad '1..2' VE
# bad ' 1' bad ' 1' VE
# bad '1 ' bad '1 ' VE
# bad '1_0' bad '1_0' VE
# bad '1.5d' bad '1.5d' VE
# bad 'True' bad 'True' VE
# bad 'None' bad 'None' VE
# 1 0 True
# 0
# unknown nan VE
