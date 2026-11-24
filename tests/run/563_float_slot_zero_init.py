# 563_float_slot_zero_init.py —— 函数内 float 槽的入口零初始化
#
# 历史 bug：`def f(): for y in [1.5, 2.5]: pass` 的入口要给 float 槽写
# const.int 0，被 store 融合成 `movq $0, %xmm8` —— x86 没有「立即数→xmm」
# 的编码，as 直接报 operand type mismatch，整个程序编译失败。
# 修复后融合判据拒绝「值与槽的寄存器类不同」，改走 StoreSlot 的
# store_slot_checked（立即数先过 %rax）；gen_const_int 也补了同样的兜底。

def f() -> None:
    for y in [1.5, 2.5]:
        pass
    print(y)

f()

def h() -> float:
    total = 0.0
    for v in [0.5, 1.5]:
        total += v
    return total

print(h())

# --- expect ---
# 2.5
# 2.0
