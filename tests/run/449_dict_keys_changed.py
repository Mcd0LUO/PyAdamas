# 449: dict 迭代「改键不改大小」（del + add）也要报错。
# CPython 3.14 对这种情况抛 RuntimeError("dictionary keys changed during iteration")，
# 而只比 len 的守卫视而不见。字典加了一个 version 字段：插入新键、删除键、
# clear 都 +1；守卫先比大小（变了报 size），大小不变再比版本（变了报 keys）。


d: dict[str, int] = {"a": 1, "b": 2}
e: dict[str, int] = {"x": 3, "y": 4}
try:
    for pair in zip(d, e):
        k = pair[0]
        del d[k]
        d["n" + k] = 1
except RuntimeError as err:
    print("RE", err)

f: dict[str, int] = {"a": 1, "b": 2}
g: dict[str, int] = {"x": 3, "y": 4}
try:
    for pair in zip(f, g):
        del f[pair[0]]
except RuntimeError as err2:
    print("RE2", err2)

h: dict[str, int] = {"a": 1}
i: dict[str, int] = {"x": 1}
out: str = ""
for pair in zip(h, i):
    out = out + pair[0]
print(out)
# --- expect ---
# RE dictionary keys changed during iteration
# RE2 dictionary changed size during iteration
# ax
