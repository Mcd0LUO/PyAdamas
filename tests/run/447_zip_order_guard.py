# 447: zip 的迭代检查按 CPython 的顺序：先问 a，再问 b。
# a 耗尽就直接结束 —— 这时 b 里改东西不该报错（CPython 根本不会碰 b）。


d1: dict[int, int] = {1: 10}
d2: dict[int, int] = {2: 20}
try:
    for a, b in zip(d1, d2):
        d2[3] = 30
    print("ok")
except RuntimeError as e:
    print("RE", e)

e1: dict[str, int] = {"a": 1, "b": 2}
f1: dict[str, int] = {"x": 1, "y": 2}
try:
    for a, b in zip(e1, f1):
        del e1[a]
except RuntimeError as err:
    print("RE2", err)

g1: dict[str, int] = {"a": 1, "b": 2}
h1: dict[str, int] = {"x": 1, "y": 2}
try:
    for a, b in zip(g1, h1):
        h1["z" + a] = 1
except RuntimeError as err2:
    print("RE3", err2)
# --- expect ---
# ok
# RE2 dictionary changed size during iteration
# RE3 dictionary changed size during iteration
