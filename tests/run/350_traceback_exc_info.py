# pyadamas: lang-gaps
"""350: P4 块3 traceback 对象 / sys.exc_info / stdlib traceback。

CPython 3.14 对拍（PYTHONNODEBUGRANGES=1 去 ~~~^^^ 锚点；锚点边界见
docs/lang-gaps-landing.md §8 块3）。避开 pa 既有边界：KeyError 的 str
引号口径、多参异常 str、raise-from 链格式化。
"""
import subprocess
import sys
import traceback


def f():
    raise ValueError("boom")


def g():
    f()


def h():
    g()


class C:
    def m(self):
        raise ValueError("method")


def reraise_mid():
    try:
        f()
    except ValueError:
        raise


def reraise_same():
    try:
        try:
            f()
        except ValueError:
            raise
    except ValueError as e:
        print(traceback.format_exc(), end="")
        print("===")


def gen():
    yield 1
    raise ValueError("gen")


def with_finally():
    try:
        f()
    finally:
        pass


def rec(n: int):
    if n == 0:
        raise ValueError("deep")
    rec(n - 1)


class MyErr(ValueError):
    pass


def show():
    print(traceback.format_exc(), end="")
    print("===")


if "--print-exc-child" in sys.argv:
    try:
        f()
    except ValueError:
        traceback.print_exc()
else:
    try:
        h()
    except ValueError:
        show()

    try:
        C().m()
    except ValueError:
        show()

    try:
        reraise_mid()
    except ValueError:
        show()

    reraise_same()

    try:
        f()
    except ValueError as e:
        try:
            raise e
        except ValueError:
            show()

    try:
        for v in gen():
            pass
    except ValueError:
        show()

    try:
        with_finally()
    except ValueError:
        show()

    try:
        rec(2)
    except ValueError:
        show()

    try:
        raise MyErr("custom")
    except MyErr:
        show()

    # ── sys.exc_info 字段面 ──────────────────────────────────────
    # args 用 except as e 绑定的静态 Exception 值读（pa 的 unknown 域
    # 没有属性面）；exc_info 的 v 过 unknown 走 str/isinstance 探针。
    try:
        f()
    except ValueError as e:
        t, v, tb = sys.exc_info()
        print(t.__name__)
        print(t is ValueError)
        print(str(v))
        print(e.args)
        print(isinstance(v, ValueError))
        print(isinstance(v, KeyError))
        print(tb is not None)
        frames = traceback.extract_tb(tb)
        print(len(frames))
        print(frames[0][2], frames[-1][2])
        print(frames[-1][3])

    # ── outside except ───────────────────────────────────────────
    t, v, tb = sys.exc_info()
    print(t is None, v is None, tb is None)
    print(traceback.format_exc(), end="")
    print(len(traceback.extract_tb(tb)))

    # ── print_exc（子进程 stderr，153 同款自执行口径）────────────
    # CPython 下 argv[0] 是 .py（不可执行），经 sys.executable 重入；
    # pa 下 argv[0] 就是编译产物，直接重入。
    if sys.argv[0].endswith(".py"):
        cmd = [sys.executable, sys.argv[0], "--print-exc-child"]
    else:
        cmd = [sys.argv[0], "--print-exc-child"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print("Traceback (most recent call last):" in r.stderr)
    print(" in f" in r.stderr)
    print("ValueError: boom" in r.stderr)
# --- expect ---
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 82, in <module>
#     h()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 22, in h
#     g()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 18, in g
#     f()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 87, in <module>
#     C().m()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 27, in m
#     raise ValueError("method")
# ValueError: method
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 92, in <module>
#     reraise_mid()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 32, in reraise_mid
#     f()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 40, in reraise_same
#     f()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 102, in <module>
#     raise e
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 99, in <module>
#     f()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 107, in <module>
#     for v in gen():
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 50, in gen
#     raise ValueError("gen")
# ValueError: gen
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 113, in <module>
#     with_finally()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 55, in with_finally
#     f()
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 118, in <module>
#     rec(2)
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 63, in rec
#     rec(n - 1)
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 63, in rec
#     rec(n - 1)
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 62, in rec
#     raise ValueError("deep")
# ValueError: deep
# ===
# Traceback (most recent call last):
#   File "/tmp/wt-lang2/tests/run/350_traceback_exc_info.py", line 123, in <module>
#     raise MyErr("custom")
# MyErr: custom
# ===
# ValueError
# True
# boom
# ('boom',)
# True
# False
# True
# 2
# <module> f
# raise ValueError("boom")
# True True True
# NoneType: None
# 0
# True
# True
# True
