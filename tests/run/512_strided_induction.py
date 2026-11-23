# 不变量步长归纳变量回归（0.448）：while 循环 q += p（p 为循环不变量/模块全局）。
# 覆盖 p=1/2/3、负步长、p=0 保护、边界、列表跨步写、嵌套外层 p 变化。

def sum_stride(n: int, p: int) -> int:
    total = 0
    q = 0
    while q < n:
        total = total + q
        q = q + p
    return total


print("p1", sum_stride(10, 1))
print("p2", sum_stride(10, 2))
print("p3", sum_stride(20, 3))
def sum_down(n: int, p: int) -> int:
    total = 0
    q = n
    while q > 0:
        total = total + q
        q = q - p
    return total


print("neg", sum_down(10, 3))
def zero_step() -> int:
    total = 0
    q = 0
    steps = 0
    while q < 100:
        total = total + 1
        steps = steps + 1
        q = q + 0
        if steps > 5:
            break
    return total


print("p0_guard", zero_step())
def boundary(n: int, p: int) -> int:
    total = 0
    q = n - 1
    while q < n:
        total = total + 1
        q = q + p
    return total


print("boundary2", boundary(10, 2), "boundary1", boundary(10, 1))
flags: list = []
for i in range(20):
    flags.append(1)
pc = 2
while pc * pc < 20:
    if flags[pc] == 1:
        qq = pc * pc
        while qq < 20:
            flags[qq] = 0
            qq = qq + pc
    pc = pc + 1
print("sieve_small", flags)
cond_sum = 0
pcond = 2
while pcond < 6:
    qcond = pcond
    while qcond < 12:
        cond_sum = cond_sum + qcond
        qcond = qcond + pcond
    pcond = pcond + 1
print("nested", cond_sum)

# --- expect ---
# p1 45
# p2 20
# p3 63
# neg 22
# p0_guard 6
# boundary2 1 boundary1 1
# sieve_small [1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1]
# nested 75
