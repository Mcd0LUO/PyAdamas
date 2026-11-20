# 同块 load 去重回归（0.335，PYADAMAS_LOOP_OPT bit2）：LICM 提升出的重复
# load.slot / load.global 合并后语义不变（nqueens row 形态 + 全局读）。
G = 7

def f(row: int, n: int) -> int:
    t = 0
    for c in range(n):
        t += row + c
        t += row - c
        t += row * 2
    return t + G

print(f(5, 4))
print(f(0, 0))
print(f(-3, 6))

# 全局在循环里多次读（无调用循环 → LICM 提升 + 去重）
def h(n: int) -> int:
    t = 0
    for i in range(n):
        t += G
        t += G * i
    return t
print(h(5))

# 槽在循环里被写：去重不得跨写合并
def w(n: int) -> int:
    x = 1
    t = 0
    for i in range(n):
        t += x
        x = x + 1
        t += x
    return t
print(w(5))

# --- expect ---
# 87
# 7
# -65
# 105
# 35
