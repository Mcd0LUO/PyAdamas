# collections 综合：Counter/defaultdict/deque 扩展
# 0.382 合并自 3 个文件：83_collections_extra.py, 84_deque.py, 88_defaultdict.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 83_collections_extra.py ──
def _case_1() -> None:
    # collections：OrderedDict（PyAdamas 的 dict 本来就保序）+ Counter 的既有行为
    import collections

    d = collections.OrderedDict()
    d["b"] = 1
    d["a"] = 2
    print(len(d))
    print(d["a"], d["b"])

    c = collections.Counter([1, 1, 2, 3, 3, 3])
    print(c[1], c[3], c[2])
    print(c.most_common(2))

# ── 原 84_deque.py ──
def _case_2() -> None:
    # collections.deque：两端进出、rotate、maxlen、迭代与 list() 转换
    import collections

    d = collections.deque()
    d.append(1)
    d.append(2)
    d.appendleft(0)
    print(len(d))
    print(d[0], d[1], d[2])
    print(d.pop())
    print(d.popleft())
    print(len(d))

    e = collections.deque([1, 2, 3])
    print(len(e))
    e.rotate(1)
    print(e[0], e[1], e[2])
    e.rotate(-1)
    print(e[0], e[1], e[2])

    m = collections.deque([], 2)
    m.append(1)
    m.append(2)
    m.append(3)
    print(len(m), m[0], m[1])

    total = 0
    for x in e:
        total += x
    print(total)

    print(list(e))

# ── 原 88_defaultdict.py ──
def _case_3() -> None:
    # collections.defaultdict：缺省下标读取按工厂造值并插入
    import collections
    from collections import defaultdict

    d = collections.defaultdict(int)
    d["a"] += 1
    d["b"] += 2
    print(d["a"], d["b"], len(d))

    e = collections.defaultdict(list)
    e["x"].append(3)
    e["x"].append(4)
    e["y"].append(5)
    print(e["x"], e["y"], len(e))

    s = collections.defaultdict(str)
    s["k"] += "v"
    print(s["k"], len(s))

    n = collections.defaultdict(None)
    n["k"] = 1
    print(n["k"], len(n))

    c = collections.defaultdict(int, {"q": 9})
    print(c["q"], c["w"], len(c))

    # get / in 不触发工厂
    g = collections.defaultdict(int)
    miss = g.get("missing")
    print(len(g), "missing" in g, miss is None)
    x = g["made"]
    print(x, len(g))

    # copy 保留工厂
    h = c.copy()
    print(h["z"], len(h))

    f = defaultdict(float)
    f["v"] += 1.5
    print(f["v"])

_case_1()
_case_2()
_case_3()

# --- expect ---
# 2
# 2 1
# 2 3 1
# [(3, 3), (1, 2)]
# 3
# 0 1 2
# 2
# 0
# 1
# 3
# 3 1 2
# 1 2 3
# 2 2 3
# 6
# [1, 2, 3]
# 1 2 2
# [3, 4] [5] 2
# v 1
# 1 1
# 9 0 2
# 0 False True
# 0 1
# 0 3
# 1.5
