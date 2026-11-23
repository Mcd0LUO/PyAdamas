# finally 里 raise（早退路径）回归（0.431）：raise 在 finally 体内、且 try 体
# 以 return / break / continue / 抛异常结束的路径上，异常必须正常传给调用方
# 的 except（修复前：被本函数自己的 cleanup landing pad 命中、未捕获退出）。

# 1) 正常结束路径（修复前就对，不能回归）
def f_norm() -> int:
    try:
        x = 1
    finally:
        raise ValueError("fin-normal")


# 2) return 路径（修复前未捕获）
def f_ret() -> int:
    try:
        return 1
    finally:
        raise ValueError("fin-ret")


# 3) break 路径
def f_break() -> int:
    s = 0
    for i in range(3):
        try:
            if i == 1:
                break
            s = s + 1
        finally:
            raise ValueError("fin-break")
    return s


# 4) continue 路径
def f_cont() -> int:
    s = 0
    for i in range(3):
        try:
            if i == 1:
                continue
            s = s + 1
        finally:
            raise ValueError("fin-cont")
    return s


# 5) try 体抛异常 + finally 再抛：新异常取代原异常
def f_orig() -> int:
    try:
        raise KeyError("orig")
    finally:
        raise ValueError("fin-orig")


class MyErr(Exception):
    pass


# 6) finally 里抛自定义异常
def f_custom() -> int:
    try:
        return 1
    finally:
        raise MyErr("fin-custom")


# 7) 嵌套 finally：内层先跑，外层 raise 覆盖
def f_nested() -> int:
    try:
        try:
            return 1
        finally:
            x = 2
    finally:
        raise ValueError("fin-nested")


class CM:
    def __enter__(self) -> "CM":
        return self

    def __exit__(self, t: str, m: str, tb: int) -> bool:
        raise ValueError("exit-raise")


# 8) with 的 __exit__ 在早退路径抛异常
def f_with() -> int:
    with CM():
        return 1


def run(label: str, k: int) -> None:
    try:
        if k == 0:
            f_norm()
        elif k == 1:
            f_ret()
        elif k == 2:
            f_break()
        elif k == 3:
            f_cont()
        elif k == 4:
            f_orig()
        elif k == 5:
            f_custom()
        elif k == 6:
            f_nested()
        else:
            f_with()
        print(label, "no-exc")
    except ValueError as e:
        print(label, "ValueError", str(e))
    except MyErr as e:
        print(label, "MyErr", str(e))
    except KeyError as e:
        print(label, "KeyError", str(e))


run("norm", 0)
run("ret", 1)
run("brk", 2)
run("cont", 3)
run("orig", 4)
run("custom", 5)
run("nested", 6)
run("with", 7)

# 9) 修好后同一函数的后续 raise 仍被外层 except 捕获（pa_eh_depth 没被多减）
def deeper(k: int) -> int:
    try:
        if k == 0:
            f_ret()
        elif k == 1:
            raise KeyError("after")
        return 0
    except KeyError:
        return 7


v = deeper(1)
print("deeper_ok", v)
try:
    deeper(0)
except ValueError as e:
    print("deeper_outer", str(e))
# 早退 + finally raise 之后再抛别的异常，仍走普通捕获
try:
    f_ret()
except ValueError:
    pass
try:
    raise IndexError("later")
except IndexError:
    print("later_ok")

# --- expect ---
# norm ValueError fin-normal
# ret ValueError fin-ret
# brk ValueError fin-break
# cont ValueError fin-cont
# orig ValueError fin-orig
# custom MyErr fin-custom
# nested ValueError fin-nested
# with ValueError exit-raise
# deeper_ok 7
# deeper_outer fin-ret
# later_ok
