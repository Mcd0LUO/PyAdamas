# 454: 被提升到 xmm 的变量槽写入「延迟常量」时不能发 `movq $0, %xmmN`
# （不是合法编码，as 直接报 operand type mismatch）。
# 触发形态：累加器在循环**体内**初始化，于是 store 的是常量 0 的位模式。


def outer() -> None:
    def f(j: int) -> float:
        return 1.0 / (j + 1)

    for j in range(3):
        total = 0.0
        total += f(j)
    print(format(total, ".12f"))

    # 整数 0 写进提升到 xmm 的槽同样是位模式（0.0）
    for j in range(2):
        acc = 0.0
        acc = acc + 1.5
    print(acc)


outer()
# --- expect ---
# 0.333333333333
# 1.5
