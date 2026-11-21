"""stdlib/decimal.py：f-string 里的 Decimal（空说明 + 纯宽度/对齐/填充）。

注意（编译器缺陷，不在本轮范围内修）：lower_joined 在类实例有 __format__
时先调用 __format__(spec)，随后又把结果当 str 再套一遍同一个 spec。纯
宽度/填充/对齐说明因为「结果已经够宽」而幂等，所以下面这些 f-string 与
format() 完全一致；带精度（.2f）、分组（,）、符号（+）、'=' 对齐、'0'
填充的 f-string 会二次套用甚至被字符串格式拒绝（ValueError），这类写法
请用 format(x, spec)。
"""
from decimal import Decimal

d: Decimal = Decimal("1234.5678")
n: Decimal = Decimal("-1234.5678")

print(f"{d}", f"{n}")
print(repr(f"{d:>16}"), repr(format(d, ">16")))
print(repr(f"{n:>16}"), repr(format(n, ">16")))
print(repr(f"{d:<16}"), repr(format(d, "<16")))
print(repr(f"{d:^16}"), repr(format(d, "^16")))
print(repr(f"{d:*^16}"), repr(format(d, "*^16")))
print(repr(f"{d:*<16}"), repr(format(d, "*<16")))
print(repr(f"{d:16}"), repr(format(d, "16")))
print(repr(f"{d:^3}"), repr(format(d, "^3")))
print(f"[{d}]", f"[{n}]")

# 带精度/分组/符号的说明：format() 正常，f-string 受上述缺陷影响。
# 例如 f"{d:.0f}" 在 pyadamas 上抛 ValueError: Invalid format specifier for str
# （__format__ 已经给出 "1235"，字符串格式又不接受 .0f），所以这里只用 format。
print(repr(format(d, ".0f")), repr(format(d, ",.2f")), repr(format(d, "+.2f")),
      repr(format(d, "020,.2f")))

# --- expect ---
# 1234.5678 -1234.5678
# '       1234.5678' '       1234.5678'
# '      -1234.5678' '      -1234.5678'
# '1234.5678       ' '1234.5678       '
# '   1234.5678    ' '   1234.5678    '
# '***1234.5678****' '***1234.5678****'
# '1234.5678*******' '1234.5678*******'
# '       1234.5678' '       1234.5678'
# '1234.5678' '1234.5678'
# [1234.5678] [-1234.5678]
# '1235' '1,234.57' '+1234.57' '0,000,000,001,234.57'
