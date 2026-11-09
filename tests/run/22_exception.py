# try/except/else/finally、raise、异常层级、with 异常路径
def classify(code: int) -> str:
    try:
        if code == 1:
            raise KeyError("k")
        if code == 2:
            raise ValueError("v")
        if code == 3:
            raise ZeroDivisionError("z")
        return "none"
    except KeyError:
        return "key"
    except (ValueError, TypeError) as e:
        return "value:" + e
    except ArithmeticError:
        return "arith"
    except Exception:
        return "other"

print(classify(1))
print(classify(2))
print(classify(3))
print(classify(0))

try:
    print("try")
except Exception:
    print("bad")
else:
    print("else")

try:
    raise ValueError("x")
except ValueError:
    print("handler")
else:
    print("bad else")

try:
    try:
        raise ValueError("inner")
    except ValueError:
        raise RuntimeError("outer")
except RuntimeError as e:
    print("nested", e)

try:
    try:
        raise KeyError("k")
    except KeyError:
        print("reraise")
        raise
except KeyError as e:
    print("bare", e)

try:
    raise ValueError(42)
except ValueError as e:
    print("int", e)

try:
    raise TypeError
except TypeError as e:
    print("empty:" + e)

try:
    assert 1 == 2, "assert msg"
except AssertionError as e:
    print("assert", e)

try:
    xs: list[int] = [1]
    print(xs[9])
except IndexError as e:
    print("index", e)

try:
    d: dict[str, int] = {"a": 1}
    print(d["b"])
except KeyError as e:
    print("key", e)

try:
    print(1 // 0)
except ZeroDivisionError as e:
    print("div", e)

def fin(n: int) -> int:
    try:
        return n + 1
    finally:
        print("fin", n)

print(fin(5))

def loop(n: int) -> int:
    total: int = 0
    for i in range(n):
        try:
            if i == 1:
                continue
            if i == 3:
                break
            total = total + i
        finally:
            total = total + 10
    return total

print(loop(5))

def through() -> None:
    try:
        raise ValueError("through")
    finally:
        print("cleanup")

try:
    through()
except ValueError as e:
    print("caught", e)

def deep(n: int) -> None:
    s: str = "a" + "b"
    if n == 0:
        raise ValueError(s)
    deep(n - 1)

try:
    deep(5)
except ValueError as e:
    print("deep", e)

class Ctx:
    def __init__(self, name: str, swallow: int) -> None:
        self.name = name
        self.swallow = swallow

    def __enter__(self):
        print("enter", self.name)
        return self

    def __exit__(self, t, m, tb):
        print("exit", self.name)
        return self.swallow == 1

try:
    with Ctx("A", 0):
        raise ValueError("boom")
except ValueError as e:
    print("caught", e)

with Ctx("B", 1):
    raise KeyError("swallow")
print("after")

try:
    with Ctx("outer", 0):
        with Ctx("inner", 0):
            raise KeyError("deep")
except KeyError as e:
    print("caught", e)

def early() -> int:
    with Ctx("E", 0):
        return 5

print(early())

class Spy:
    def __init__(self, swallow: int) -> None:
        self.swallow = swallow

    def __enter__(self):
        return self

    def __exit__(self, t: str, m: str, tb: str):
        print("spy", t, m)
        return self.swallow == 1

try:
    with Spy(0):
        raise ValueError("v")
except ValueError as e:
    print("caught", e)

with Spy(1):
    raise KeyError("k")
print("suppressed")
# --- expect ---
# key
# value:v
# arith
# none
# try
# else
# handler
# nested outer
# reraise
# bare k
# int 42
# empty:
# assert assert msg
# index list index out of range
# key key not found
# div integer division or modulo by zero
# fin 5
# 6
# 42
# cleanup
# caught through
# deep ab
# enter A
# exit A
# caught boom
# enter B
# exit B
# after
# enter outer
# enter inner
# exit inner
# exit outer
# caught deep
# enter E
# exit E
# 5
# spy ValueError v
# caught v
# spy KeyError k
# suppressed
