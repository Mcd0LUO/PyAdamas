# 436: except 体里 raise（显式或裸重抛）必须先跑本 try 的 finally。
# 进入 handler 时已经 pa_eh_leave 出了保护区，handler 体里再 raise 不会经过
# 本 try 的 landing pad，所以 finally 得靠 handler 体降级时登记的内联体来跑。


def explicit() -> None:
    try:
        raise ValueError("v")
    except ValueError:
        raise KeyError("k")
    finally:
        print("A-fin")


def bare() -> None:
    try:
        raise ValueError("v")
    except ValueError:
        raise
    finally:
        print("B-fin")


def nested() -> None:
    try:
        try:
            raise ValueError("v")
        except ValueError:
            raise KeyError("k")
        finally:
            print("C-inner-fin")
    finally:
        print("C-outer-fin")


def from_cause() -> None:
    try:
        raise ValueError("v")
    except ValueError as e:
        raise KeyError("k") from e
    finally:
        print("D-fin")


def ret_in_except() -> int:
    try:
        raise ValueError("v")
    except ValueError:
        return 7
    finally:
        print("E-fin")


for fn in (explicit, bare, nested, from_cause):
    try:
        fn()
    except BaseException as e:
        print("caught", type(e).__name__)
print(ret_in_except())
# --- expect ---
# A-fin
# caught KeyError
# B-fin
# caught ValueError
# C-inner-fin
# C-outer-fin
# caught KeyError
# D-fin
# caught KeyError
# E-fin
# 7
