# case None 在 Optional 主体上必须能匹配真正的 None。
# 降级期先把 Optional[T] 剥成 T 再判断「主体类型是不是 None 类」，于是
# int|None 主体永远走 jump_fail，None 落到 case _。非 Optional 的 int 主体
# 仍然不匹配 None（与 CPython 一致）。

def opt_none(x: int | None) -> str:
    match x:
        case None:
            return "none"
        case _:
            return "other"

def opt_zero(x: int | None) -> str:
    match x:
        case 0:
            return "zero"
        case _:
            return "other"

def plain_int(v: int) -> str:
    match v:
        case None:
            return "none-pattern"
        case _:
            return "other"

print(opt_none(None), opt_none(5))
print(opt_zero(0))
print(plain_int(0))
# --- expect ---
# none other
# zero
# other
