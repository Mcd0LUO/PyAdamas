# 异常语义回归（0.430）：try/except/else/finally 主干、finally 与控制流、
# 异常对象/自定义异常、内置异常触发点、以及 -O2 DCE 不再吞掉会抛异常的 int(float)。

# 1) 多分支 except / 元组 except / else / finally
def f(n: int) -> int:
    try:
        if n == 1:
            raise ValueError("one")
        if n == 2:
            raise KeyError("two")
        return 10
    except ValueError:
        return 1
    except (KeyError, IndexError):
        return 2
    else:
        return 0
    finally:
        print("fin", n)


r0 = f(0)
r1 = f(1)
r2 = f(2)
print("f", r0, r1, r2)
try:
    raise ValueError("bare")
except ValueError as e:
    print("as_e", str(e))

# 2) finally：正常路径、异常路径、return 穿过 finally
def g(n: int) -> int:
    try:
        return 1 // n
    finally:
        print("g_fin", n)


gr = g(2)
print("g", gr)
try:
    g(0)
except ZeroDivisionError:
    print("g_caught")
def p() -> int:
    try:
        raise ValueError("orig")
    finally:
        print("p_fin")


try:
    p()
except ValueError:
    print("p_caught")

# 3) 嵌套 try + 内层 finally 后外层捕获
def nested(n: int) -> int:
    try:
        try:
            if n == 0:
                raise ValueError("inner")
            return 1
        finally:
            print("inner_fin", n)
    except ValueError:
        return 2


nr0 = nested(0)
nr1 = nested(1)
print("nested", nr0, nr1)

# 4) 自定义异常 + 父类捕获
class MyErr(Exception):
    pass


try:
    raise MyErr("custom")
except Exception:
    print("custom_parent")
try:
    raise MyErr("child")
except MyErr as e:
    print("custom_child", str(e))

# 5) 循环里的 finally（continue 路径也跑）
for i in range(3):
    try:
        if i == 1:
            continue
        print("loop", i)
    finally:
        print("loop_fin", i)

# 6) 内置异常触发点（只钉类型，不钉文案）
def trig(k: int) -> str:
    try:
        if k == 0:
            x = 1 // 0
        elif k == 1:
            x = [][0]
        elif k == 2:
            d = {}
            x = d["k"]
        elif k == 3:
            x = int("abc")
        elif k == 4:
            x = int(float("inf"))
        else:
            x = 0
        return "ok"
    except ZeroDivisionError:
        return "ZeroDivisionError"
    except IndexError:
        return "IndexError"
    except KeyError:
        return "KeyError"
    except ValueError:
        return "ValueError"
    except OverflowError:
        return "OverflowError"


print("trig", trig(0), trig(1), trig(2), trig(3), trig(4), trig(5))

# 7) -O2 DCE 回归：语句形态（结果没人用）的 int(float("inf")) 仍必须抛
caught = 0
try:
    int(float("inf"))
except OverflowError:
    caught = 1
print("dce_overflow", caught)
try:
    int(float("nan"))
except ValueError:
    print("dce_nan_ok")

# --- expect ---
# fin 0
# fin 1
# fin 2
# f 10 1 2
# as_e bare
# g_fin 2
# g 0
# g_fin 0
# g_caught
# p_fin
# p_caught
# inner_fin 0
# inner_fin 1
# nested 2 1
# custom_parent
# custom_child child
# loop 0
# loop_fin 0
# loop_fin 1
# loop 2
# loop_fin 2
# trig ZeroDivisionError IndexError KeyError ValueError OverflowError ok
# dce_overflow 1
# dce_nan_ok
