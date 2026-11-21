# 418: f-string 对类实例只格式化一次（0.360）。
# 之前走完 __format__ 后又把结果当字符串套一遍 FmtStr：带精度/分组/符号的
# 说明符会直接报错，纯宽度的说明符会重复补白。
from decimal import Decimal


class Money:
    cents: int

    def __init__(self, cents: int) -> None:
        self.cents = cents

    def __format__(self, spec: str) -> str:
        return "M" + spec + str(self.cents)

    def __str__(self) -> str:
        return "money"


m = Money(7)
print(f"{m}")
print(f"{m:.0f}")
print(f"{m:+.0f}")
print(f"{m:,}")
print(format(m, ".0f"))

d = Decimal("1.25")
print(f"{d:.1f}")
print(f"[{d:<10}]".replace(" ", "."))
print(f"[{d:^10}]".replace(" ", "."))
print(f"{d}")

n = 42
print(f"{n:>6}")
s = "ab"
print(f"{s:>5}")
obj: str = f"{n}-{s}"
print(obj)
# --- expect ---
# M7
# M.0f7
# M+.0f7
# M,7
# M.0f7
# 1.2
# [1.25......]
# [...1.25...]
# 1.25
#     42
#    ab
# 42-ab
