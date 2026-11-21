# 402: 循环内早返回的槽释放（0.358 修「早返回漏 dec」）。
# 触发形状：显式 object 槽的**首次**赋值在循环体内、源码序位于早返回之后 ——
# 单遍降级发射那条 return 的出口释放时，该槽的 slot_owned 还没置位，
# 而循环次轮起槽里已是上一轮 retain 过的堆值，于是永久漏一份引用。
def relay(x: object) -> object:
    return x


def early(n: int, stop: bool) -> int:
    for i in range(n):
        if i > 0 and stop:
            return 1
        cur: object = relay("value-" + str(i))
        if cur == "value-9":
            return 2
    return 0


def raises(n: int, stop: bool) -> str:
    out: str = "-"
    for i in range(n):
        if i > 0 and stop:
            raise ValueError("early")
        cur: object = relay("item-" + str(i))
        out = str(cur)
    return out


print(early(0, True))
print(early(3, True))
print(early(3, False))
print(raises(0, True))
print(raises(2, False))
try:
    raises(3, True)
except ValueError as e:
    print("caught", e)
# --- expect ---
# 0
# 1
# 0
# -
# item-1
# caught early
