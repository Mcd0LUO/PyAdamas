# 动态调用传浮点参数：浮点值必须按 xmm 传，不能当整数走整型实参寄存器。
#
# 修复前 functools.reduce 只看结果域选实现：结果域是 unknown（裸 list 形参、
# 未标注返回）时走整型版，浮点元素的位模式被当整数塞进 %rdi，静默算错
# （reduce(fadd, [1.5, 2.5]) 打印 4612811918334230528 而不是 4.0）。
# 现在按回调形参域选实现：两个形参都是 float 就走 double 版。
import functools


def fadd(a: float, b: float) -> float:
    return a + b


def show(a: float, b: float) -> None:
    print("show", a + b)


# 裸 list 形参：元素域 unknown，只有回调签名能定传参域。
def fold(xs: list) -> float:
    return functools.reduce(fadd, xs)


print("fold", fold([1.5, 2.5]))
print("fold3", fold([1.5, 2.5, 3.5]))
print("fold1", fold([2.5]))


# 结果域 unknown 也要走对（修复前这条打印 4616189618054758400）。
def fold_unknown(xs: list):
    return functools.reduce(fadd, xs)


print("fold_unknown", fold_unknown([1.5, 2.5]))


# list[int] + 浮点回调：元素逐元素提升成 double 再折叠。
def fold_int(xs: list[int]) -> float:
    return functools.reduce(fadd, xs)


print("fold_int", fold_int([1, 2]))


# list[float] 形参（修复前本来就对，钉住不回归）。
def fold_float(xs: list[float]) -> float:
    return functools.reduce(fadd, xs)


print("fold_float", fold_float([1.5, 2.5]))


# 带初始值。
def fold_init(xs: list) -> float:
    return functools.reduce(fadd, xs, 10.0)


print("fold_init", fold_init([1.5, 2.5]))


# 未标注形参类型（完全 unknown 的入口）。
def fold_untyped(xs) -> float:
    return functools.reduce(fadd, xs)


print("fold_untyped", fold_untyped([1.5, 2.5]))


# 直接的 unknown 函数值调用：函数值从裸 list 取出，静态签名不可见。
# 调用点实参是 float 变量，值按 xmm 传，结果由被调函数自己打印。
fs: list = [show]
k = fs[0]
k(1.5, 2.5)
k(2.5, 2.5)

# 同一函数被两种域调用：int 列表仍按整数语义折叠。
def addi(a: int, b: int) -> int:
    return a + b


def fold_ints(xs: list) -> int:
    return functools.reduce(addi, xs)


print("fold_ints", fold_ints([1, 2, 3]))

# 字符串回调不受影响。
def sadd(a: str, b: str) -> str:
    return a + b


def fold_strs(xs: list) -> str:
    return functools.reduce(sadd, xs)


print("fold_strs", fold_strs(["a", "b", "c"]))
# --- expect ---
# fold 4.0
# fold3 7.5
# fold1 2.5
# fold_unknown 4.0
# fold_int 3.0
# fold_float 4.0
# fold_init 14.0
# fold_untyped 4.0
# show 4.0
# show 5.0
# fold_ints 6
# fold_strs abc
