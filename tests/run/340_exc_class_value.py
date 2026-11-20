# pyadamas: lang-gaps
"""340: P4 exception classes as values + contextlib.suppress/closing.

CPython parity: every printed line is byte-identical to CPython 3.14.
Avoided pa boundaries (documented in docs/lang-gaps-landing.md):
multi-arg exception str() quoting, binding None __enter__ result with
'as', attribute access on exception instances.
"""
from contextlib import suppress, closing


class MyErr(ValueError):
    pass


class Resource:
    closed: bool

    def __init__(self) -> None:
        self.closed = False

    def close(self) -> None:
        self.closed = True


def show(x: unknown) -> str:
    return str(x)


def show_repr(x: unknown) -> str:
    return repr(x)


# ── builtin exception classes as values ─────────────────────────
print(ValueError)
print(IndexError)
print(repr(KeyError))
print(str(TypeError))
print(ValueError.__name__)
print(IndexError.__name__)
E = ValueError
print(E is ValueError)
print(E is IndexError)
print(show(ValueError))
print(show_repr(IndexError))
xs: list[unknown] = [ValueError, IndexError, KeyError]
print(len(xs))
for c in xs:
    print(show(c))
print(xs[1] is IndexError)

# ── custom exception class as value ─────────────────────────────
# repr(user class) shows the last name segment in pa (pre-existing
# pa_class_repr behavior for ALL user classes) — not exercised here.
print(MyErr.__name__)
M = MyErr
print(M is MyErr)
print(show(MyErr.__name__))

# ── isinstance / issubclass ─────────────────────────────────────
try:
    raise ValueError("boom")
except ValueError as e:
    print(isinstance(e, ValueError))
    print(isinstance(e, IndexError))
    print(isinstance(e, Exception))
    print(isinstance(e, (IndexError, ValueError)))
    print(str(e))
try:
    raise MyErr("sub")
except ValueError as e:
    print(isinstance(e, MyErr))
    print(isinstance(e, ValueError))
    print(str(e))
print(issubclass(ValueError, Exception))
print(issubclass(ValueError, IndexError))
print(issubclass(ValueError, ValueError))
print(issubclass(IndexError, LookupError))
print(issubclass(MyErr, ValueError))
print(issubclass(MyErr, Exception))
print(issubclass(MyErr, IndexError))

# ── raise via class value ───────────────────────────────────────
try:
    raise E("via-value")
except ValueError as e:
    print("caught", str(e))
try:
    raise E
except ValueError as e:
    print("bare", str(e))
try:
    raise M("custom-via-value")
except MyErr as e:
    print("caught-custom", str(e))

# ── suppress ────────────────────────────────────────────────────
with suppress(ValueError):
    raise ValueError("suppressed")
print("after suppress")

with suppress(ValueError, IndexError):
    raise IndexError("suppressed2")
print("after suppress2")

with suppress(ValueError):
    pass
print("normal path")

try:
    with suppress():
        raise ValueError("zero-arg")
except ValueError as e:
    print("zero-arg propagated", str(e))

try:
    with suppress(ValueError):
        raise IndexError("not suppressed")
except IndexError as e:
    print("propagated", str(e))

with suppress(ValueError):
    with suppress(IndexError):
        raise IndexError("inner")
    raise ValueError("outer")
print("nested ok")

with suppress(MyErr):
    raise MyErr("custom suppressed")
print("custom suppressed")

with suppress(ValueError):
    raise MyErr("subclass suppressed")
print("subclass suppressed")


def raiser() -> None:
    raise KeyError("from-function")


with suppress(KeyError):
    raiser()
print("function raise suppressed")

# state after suppression is clean
try:
    raise ValueError("again")
except ValueError as e:
    print("still works", str(e))

# ── closing ─────────────────────────────────────────────────────
r = Resource()
with closing(r):
    print("in closing", r.closed)
print("after closing", r.closed)

r2 = Resource()
try:
    with closing(r2):
        raise ValueError("x")
except ValueError:
    print("exc path closed", r2.closed)

f = open("340_tmp.txt", "w")
with closing(f):
    f.write("hello")
print("file closed", f.closed)
g = open("340_tmp.txt", "r")
print("content", g.read())
g.close()

import os
os.remove("340_tmp.txt")
print("done")
# --- expect ---
# <class 'ValueError'>
# <class 'IndexError'>
# <class 'KeyError'>
# <class 'TypeError'>
# ValueError
# IndexError
# True
# False
# <class 'ValueError'>
# <class 'IndexError'>
# 3
# <class 'ValueError'>
# <class 'IndexError'>
# <class 'KeyError'>
# True
# MyErr
# True
# MyErr
# True
# False
# True
# True
# boom
# True
# True
# sub
# True
# False
# True
# True
# True
# True
# False
# caught via-value
# bare 
# caught-custom custom-via-value
# after suppress
# after suppress2
# normal path
# zero-arg propagated zero-arg
# propagated not suppressed
# nested ok
# custom suppressed
# subclass suppressed
# function raise suppressed
# still works again
# in closing False
# after closing True
# exc path closed True
# file closed True
# content hello
# done
