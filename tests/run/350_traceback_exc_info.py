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
        print(traceback.format_exc().replace(__file__, "350_traceback_exc_info.py"), end="")
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


class Ctx:
    def __enter__(self):
        return self

    def __exit__(self, et, ev, tb):
        return False


def show():
    print(traceback.format_exc().replace(__file__, "350_traceback_exc_info.py"), end="")
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
    print(traceback.format_exc().replace(__file__, "350_traceback_exc_info.py"), end="")
    print(len(traceback.extract_tb(tb)))

    # ── with 块穿过的异常（with 帧不进 tb，CPython 口径）─────────
    try:
        with Ctx():
            f()
    except ValueError:
        show()

    # ── tb 跨 GC：持有 tb 期间大量分配（含环）触发主收集，帧链仍可读 ──
    try:
        f()
    except ValueError:
        t, v, tb = sys.exc_info()
        keep: list[unknown] = []
        for i in range(3000):
            cyc: list[unknown] = [i]
            cyc.append(cyc)
            keep.append(cyc)
        frames = traceback.extract_tb(tb)
        print(len(frames))
        print(frames[-1][2])

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
#   File "350_traceback_exc_info.py", line 90, in <module>
#     h()
#   File "350_traceback_exc_info.py", line 22, in h
#     g()
#   File "350_traceback_exc_info.py", line 18, in g
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 95, in <module>
#     C().m()
#   File "350_traceback_exc_info.py", line 27, in m
#     raise ValueError("method")
# ValueError: method
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 100, in <module>
#     reraise_mid()
#   File "350_traceback_exc_info.py", line 32, in reraise_mid
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 40, in reraise_same
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 110, in <module>
#     raise e
#   File "350_traceback_exc_info.py", line 107, in <module>
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 115, in <module>
#     for v in gen():
#   File "350_traceback_exc_info.py", line 50, in gen
#     raise ValueError("gen")
# ValueError: gen
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 121, in <module>
#     with_finally()
#   File "350_traceback_exc_info.py", line 55, in with_finally
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 126, in <module>
#     rec(2)
#   File "350_traceback_exc_info.py", line 63, in rec
#     rec(n - 1)
#   File "350_traceback_exc_info.py", line 63, in rec
#     rec(n - 1)
#   File "350_traceback_exc_info.py", line 62, in rec
#     raise ValueError("deep")
# ValueError: deep
# ===
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 131, in <module>
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
# Traceback (most recent call last):
#   File "350_traceback_exc_info.py", line 163, in <module>
#     f()
#   File "350_traceback_exc_info.py", line 14, in f
#     raise ValueError("boom")
# ValueError: boom
# ===
# 2
# f
# True
# True
# True
