# 239_p3_minor —— 0.317：P3 三件 + list[list] 自然排序（N6/N7/N9/同族）
#
# 0.316 及以前：
#   * N6  dict(x=1, y=2) 的静态类型是 dict[unknown, unknown] → 打印成
#         "<dict object>"，下标读回走 unknown 路径。
#   * N7  filter(None, 异构列表) 保留空串（unknown 真值按「非零」判），
#         且元素打印没有 repr 引号。
#   * N9  sum(list[float]) 朴素累加（CPython 3.12+ 是 Neumaier 补偿求和）。
#   * list[list] 的自然排序是 no-op（kind-3 机器字，比堆地址）。
# 本用例对拍 CPython，O0/O1/O2 全一致。

# ── 1) dict(**kwargs) 的静态类型（N6）───────────────────────────
print(dict(x=1, y=2))
print(dict(a="x", b="y"))
d: dict[str, int] = dict(p=5, q=7)
print(d, d["p"] + d["q"], len(d))
print(dict())
e = dict(k="v")
print(e["k"] + "!")
print(sorted(dict(a=1, b=2).keys()))

# ── 2) filter(None, 异构) / unknown 真值（N7）───────────────────
xs: list[object] = [0, 1, "", "a", 2]
print(list(filter(None, xs)))
ys: list[str] = ["", "a", "", "b"]
print(list(filter(None, ys)))
zs: list[object] = [0, 0.0, "x", 1]
print(list(filter(None, zs)))
# bool() 对 unknown 同样是 Python 真值
for v in xs:
    print(bool(v))
n: object = ""
print(bool(n), bool("a"), bool(0), bool(2))

# ── 3) sum(list[float]) 用 CPython 的 Neumaier 补偿求和（N9）─────
print(sum([0.1] * 10))
print(sum([1e16, 1.0, -1e16]))
print(sum([0.1, 0.2, 0.3], 0.0))
print(sum([1e300, 1e300, -1e300]))
print(sum([2.0**53, 1.0, 1.0]))
fs: list[float] = []
for i in range(1000):
    fs.append(1.0 / (i + 1))
print(sum(fs))
print(sum([1, 2, 3]), sum([1.5, 2.5]), sum([]), sum([0.5]))

# ── 4) list[list] 自然排序（kind-4 描述符）───────────────────────
rows: list[list[int]] = [[2, 9], [1, 5], [2, 3], [1, 5]]
rows.sort()
print(rows)
w: list[list[str]] = [["b", "x"], ["a", "z"], ["a", "y"]]
print(sorted(w))
m: list[list[float]] = [[1.5, 2.0], [1.5, 1.0], [0.5, 9.0]]
print(sorted(m))
# 嵌套两层
deep: list[list[list[int]]] = [[[2, 1]], [[1, 2]], [[1, 1]]]
print(sorted(deep))
# 空内层列表：前缀相等短者小
sp: list[list[int]] = [[1, 2], [1], [], [1, 2, 3]]
sp.sort()
print(sp)
# --- expect ---
# {'x': 1, 'y': 2}
# {'a': 'x', 'b': 'y'}
# {'p': 5, 'q': 7} 12 2
# {}
# v!
# ['a', 'b']
# [1, 'a', 2]
# ['a', 'b']
# ['x', 1]
# False
# True
# False
# True
# True
# False True False True
# 1.0
# 1.0
# 0.6
# 1e+300
# 9007199254740994.0
# 7.485470860550345
# 6 4.0 0 0.5
# [[1, 5], [1, 5], [2, 3], [2, 9]]
# [['a', 'y'], ['a', 'z'], ['b', 'x']]
# [[0.5, 9.0], [1.5, 1.0], [1.5, 2.0]]
# [[[1, 1]], [[1, 2]], [[2, 1]]]
# [[], [1], [1, 2], [1, 2, 3]]
