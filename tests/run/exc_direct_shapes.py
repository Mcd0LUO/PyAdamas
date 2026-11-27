# pyadamas: exc-direct
# 方案 A 第二步（raise 直接跳 land_direct）的语义对拍：门控 PYADAMAS_EXC_DIRECT
# 开关下输出必须一致，且与 CPython 一致。覆盖：裸 except / except Exception /
# except 具体类（匹配与不匹配重抛）/ finally / 嵌套 try / handler 里再 raise /
# raise ... from / 直接跳捕获后再抛（pa_eh_depth 回归）/ 递归。
# 门控默认关；CI 里本用例同时覆盖「门控关」的语义，门控开在专项验证里跑。
class MyError(Exception):
    pass

class OtherError(Exception):
    pass

def thrower(x: int) -> int:
    if x % 2 == 0:
        raise MyError("even")
    return x

def thrower_key(x: int) -> int:
    if x % 2 == 1:
        raise KeyError("odd")
    return x

# 1) 裸 except + 同函数 raise
def p1(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            if i % 2 == 0:
                raise MyError("x")
        except:
            c += 1
        i += 1
    return c
print("p1", p1(10))

# 2) 裸 except + 跨函数 raise（内联后直接跳）
def p2(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += thrower(i)
        except:
            c += 100
        i += 1
    return c
print("p2", p2(10))

# 3) except Exception + 匹配
def p3(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += thrower(i)
        except Exception:
            c += 100
        i += 1
    return c
print("p3", p3(10))

# 4) except 具体类 + 匹配（运行期匹配仍在分发链里）
def p4(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += thrower(i)
        except MyError:
            c += 7
        i += 1
    return c
print("p4", p4(10))

# 5) except 具体类 + 不匹配 → 重抛到外层裸 except
def p5(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                c += thrower(i)
            except KeyError:
                c += 1000
        except:
            c += 3
        i += 1
    return c
print("p5", p5(10))

# 6) finally（老路）：finally 必须执行
def p6(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            if i % 2 == 0:
                raise MyError("x")
        except:
            c += 1
        finally:
            c += 10
        i += 1
    return c
print("p6", p6(10))

# 7) 嵌套 try：内层 typed 匹配、外层裸
def p7(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                c += thrower_key(i)
            except KeyError:
                c += 11
        except:
            c += 100
        i += 1
    return c
print("p7", p7(10))

# 8) handler 里再 raise（老路）
def p8(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                c += thrower(i)
            except MyError:
                raise OtherError("from handler")
        except OtherError:
            c += 5
        i += 1
    return c
print("p8", p8(10))

# 9) raise ... from（老路）
def p9(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                raise MyError("cause")
            except MyError:
                raise OtherError("outer") from None
        except OtherError:
            c += 9
        i += 1
    return c
print("p9", p9(6))

# 10) pa_eh_depth 回归：直接跳捕获后，后续 C 运行时异常/正常 try 必须仍被捕获
def p10() -> int:
    c = 0
    try:
        raise MyError("first")
    except:
        c += 1
    try:
        xs = [1, 2, 3]
        c += xs[99]
    except:
        c += 2
    try:
        raise KeyError("later")
    except KeyError:
        c += 4
    return c
print("p10", p10())

# 11) 递归：每层自己的保护区
def p11(n: int) -> int:
    c = 0
    try:
        if n > 0:
            c += p11(n - 1)
        else:
            raise MyError("leaf")
    except:
        c += 1
    return c
print("p11", p11(5))

# 12) 生成器里的 try（可挂起函数无零开销保护区，永久老路）
def gen_worker(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            if i % 2 == 0:
                raise MyError("g")
        except:
            c += 1
        i += 1
        yield c
    return c

def p12(n: int) -> int:
    total = 0
    for v in gen_worker(n):
        total += v
    return total
print("p12", p12(5))

# 13) 子类匹配：except MyError 必须接住 raise Sub(...)
class Sub(MyError):
    pass

def throw_sub(x: int) -> int:
    if x % 2 == 0:
        raise Sub("sub")
    return x

def p13(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += throw_sub(i)
        except MyError:
            c += 1
        i += 1
    return c
print("p13", p13(6))

# 14) except Exception 不接 BaseException 子类（KeyboardInterrupt）：外层裸 except 接
def p14(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                raise KeyboardInterrupt
            except Exception:
                c += 1000
        except:
            c += 1
        i += 1
    return c
print("p14", p14(3))

# 15) typed 不匹配 → 重抛到**调用方**的 try
def inner_nomatch(x: int) -> int:
    try:
        raise MyError("m")
    except KeyError:
        return 1000
    return x

def p15(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += inner_nomatch(i)
        except MyError:
            c += 1
        i += 1
    return c
print("p15", p15(4))

# 16) 元组 handler（单 handler 多类型）：匹配
class TupA(Exception):
    pass

class TupB(Exception):
    pass

def throw_tup(x: int) -> int:
    if x % 2 == 0:
        raise TupA("a")
    if x % 3 == 0:
        raise TupB("b")
    return 0

def p16(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += throw_tup(i)
        except (TupA, TupB):
            c += 10
        i += 1
    return c
print("p16", p16(6))

# 17) 元组 handler：不匹配 → 重抛到外层
def throw_other(x: int) -> int:
    if x % 2 == 0:
        raise OtherError("o")
    return x

def p17(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                c += throw_other(i)
            except (TupA, TupB):
                c += 1000
        except OtherError:
            c += 1
        i += 1
    return c
print("p17", p17(6))

# 18) 值 raise（内置异常）：e.args / str(e) / type(e).__name__ 与老路逐字节一致
def p18(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            raise ValueError("vx")
        except ValueError as e:
            c += 100
            c += len(e.args)
            c += len(str(e))
            if type(e).__name__ == "ValueError":
                c += 1000
        i += 1
    return c
print("p18", p18(3))

# 19) 跨函数值 raise（内联后直接跳 prep_value）
def value_throw(x: int) -> int:
    if x % 2 == 0:
        raise ValueError("vx")
    return x

def p19(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += value_throw(i)
        except ValueError as e:
            c += 10
            c += len(e.args)
        i += 1
    return c
print("p19", p19(6))

# 20) 富异常实例跨函数 raise（内联后直接跳 prep_instance）
class Rich(Exception):
    def __init__(self, m: str) -> None:
        self.m = m

    def text(self) -> str:
        return self.m

def rich_throw(x: int) -> int:
    if x % 2 == 0:
        raise Rich("rich")
    return x

def p20(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            c += rich_throw(i)
        except Rich as e:
            c += len(e.text())
        i += 1
    return c
print("p20", p20(6))

# 21) raise ... from None（无 cause 链：走 prep_value，语义与老路一致）
def p21(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                raise ValueError("x")
            except ValueError:
                raise KeyError("y") from None
        except KeyError as e:
            c += 1
            if e.__cause__ is None:
                c += 10
        i += 1
    return c
print("p21", p21(3))

# 22) raise ... from e（真实 cause 链：必须走老路，__cause__ 保留）
def p22(n: int) -> int:
    c = 0
    i = 0
    while i < n:
        try:
            try:
                raise ValueError("src")
            except ValueError as e:
                raise KeyError("wrap") from e
        except KeyError as e2:
            c += 1
            if e2.__cause__ is not None:
                c += 10
        i += 1
    return c
print("p22", p22(3))

# --- expect ---
# p1 5
# p2 525
# p3 525
# p4 60
# p5 40
# p6 105
# p7 75
# p8 50
# p9 54
# p10 7
# p11 1
# p12 9
# p13 12
# p14 3
# p15 4
# p16 40
# p17 12
# p18 3309
# p19 42
# p20 21
# p21 33
# p22 33
