# try 体内 float 变量：赋值后在同一 try 体内使用（阶段 B5 修的既有 bug）。
#
# 根因：TypeChecker::stmt_try 会把「try 体内首次赋值」的变量的静态类型与
# try 入口（此时是 hoisted unknown）并成 unknown，于是槽按 unknown 存、float
# 要装箱成 CPYFLOAT；但 try 体内使用点仍按具体类型（float）降级 —— 读的时候
# 必须按存储类型解盒（AnyToF64），否则装箱字被当 double 用（打印 5e-310 垃圾）。
#
# 覆盖：最小形状、同一体内多次赋值、算术/比较/f-string 中使用、try/except/
# finally 与嵌套 try、int/str/list 对照、异常真的抛出后 handler 读值、
# try 内 return、闭包（cell）读取、模块级 global、以及「try 体后使用」。

def minimal() -> None:
    try:
        x = 2.0 + 3.0
        print("min", x)
    except ZeroDivisionError:
        print("min-caught")


def expr_use() -> None:
    try:
        x = 2.0 + 3.0
        print("expr", x + 1.0, x * 2.0, x > 4.0, -x)
    except ZeroDivisionError:
        print("expr-caught")


def multi_assign() -> None:
    try:
        x = 1.0
        x = x + 1.0
        x = x * 3.0
        print("multi", x)
    except ZeroDivisionError:
        print("multi-caught")


def finally_case() -> None:
    try:
        x = 2.0 + 3.0
        print("fin", x)
    except ZeroDivisionError:
        print("fin-caught")
    finally:
        print("fin-finally")


def nested_case() -> None:
    try:
        x = 1.5 + 1.0
        try:
            y = x * 2.0
            print("nest", x, y)
        except ValueError:
            print("nest-inner")
    except ZeroDivisionError:
        print("nest-outer")


def int_case() -> None:
    try:
        n = 2 + 3
        print("int", n, n + 1)
    except ZeroDivisionError:
        print("int-caught")


def str_case() -> None:
    try:
        s = "ab" + "cd"
        print("str", s)
    except ZeroDivisionError:
        print("str-caught")


def list_case() -> None:
    try:
        v = [1.0, 2.0]
        print("list", v[1])
    except ZeroDivisionError:
        print("list-caught")


def fstring_case() -> None:
    try:
        x = 2.0 + 3.0
        print(f"fs {x}")
    except ZeroDivisionError:
        print("fs-caught")


def except_reads_value() -> None:
    try:
        x = 6.0 * 1.5
        raise ValueError("boom")
    except ValueError:
        print("exc", x)


def return_in_try(flag: bool) -> float:
    try:
        x = 2.0 + 3.0
        if flag:
            return x * 2.0
    except ZeroDivisionError:
        return -1.0
    return x


def cell_case() -> None:
    try:
        x = 2.0 + 3.0

        def inner() -> None:
            print("cell", x + 1.0)

        inner()
    except ZeroDivisionError:
        print("cell-caught")


def global_case() -> None:
    global gx
    try:
        gx = 2.0 + 3.0
        print("global", gx)
    except ZeroDivisionError:
        print("global-caught")


def after_try() -> None:
    try:
        x = 2.0 + 3.0
    except ZeroDivisionError:
        print("after-caught")
    y = x + 1.0
    print("after", x, y)


minimal()
expr_use()
multi_assign()
finally_case()
nested_case()
int_case()
str_case()
list_case()
fstring_case()
except_reads_value()
r1 = return_in_try(True)
r2 = return_in_try(False)
print("ret", r1, r2)
cell_case()
global_case()
after_try()
print("global-after", gx)

# --- expect ---
# min 5.0
# expr 6.0 10.0 True -5.0
# multi 6.0
# fin 5.0
# fin-finally
# nest 2.5 5.0
# int 5 6
# str abcd
# list 2.0
# fs 5.0
# exc 9.0
# ret 10.0 5.0
# cell 6.0
# global 5.0
# after 5.0 6.0
# global-after 5.0
