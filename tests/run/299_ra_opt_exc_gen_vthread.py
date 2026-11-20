# RA_OPT 正确性命门回归（0.336）：异常展开 / 生成器挂起 / vthread 切换下
# callee-saved 保存恢复。has_handlers / gen_ 函数整函数跳过提升（现有门槛），
# 只读参数副本在普通函数里别名后跨调用/跨挂起必须值正确。
def gen(n: int):
    acc = 0
    for i in range(n):
        acc += i
        yield acc


for x in gen(6):
    print(x)


def risky(n: int) -> int:
    t = 0
    for i in range(n):
        try:
            if i == 3:
                raise ValueError("x")
            t += i
        except ValueError:
            t += 100
    return t


print(risky(6))


def holder(ch, row: int, n: int):
    s = 0
    for c in range(n):
        s += row + c
    ch.send(s)
    ch.recv()
    print("vt", s, row, n)


ch = make_chan(int, 0)
h = go holder(ch, 7, 5)
v = ch.recv()
ch.send(1)
join(h)
print("done", v)
# --- expect ---
# 0
# 1
# 3
# 6
# 10
# 15
# 112
# vt 45 7 5
# done 45
