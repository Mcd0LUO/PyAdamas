# 浮点槽到槽的搬移：SSE 的 movsd 不支持内存到内存。
# 以前 move_fp 在源和目的都在栈槽时直接发 movsd mem, mem，汇编器报
# "operand size mismatch"，整个文件汇编失败（不是运行期错误，是编译失败）。
# 触发条件：同一个变量先被推断/赋成浮点，再在热循环里做槽到槽的搬移。

best = 10 ** 9
d = 1.5
if d < best:
    best = d
print(best)


# 循环里反复做 int -> float 的槽搬移
def narrow(n: int) -> float:
    smallest = 1e9
    i: int = 0
    while i < n:
        x = float(i)
        if x < smallest:
            smallest = x
        i = i + 1
    return smallest


print(narrow(5))


# int/float 混用后再比较
def mixed(a: int, b: float) -> float:
    m = 10 ** 9
    if b < m:
        m = b
    if a < m:
        m = float(a)
    return m


print(mixed(3, 7.5))

# --- expect ---
# 1.5
# 0.0
# 3.0
