# fuse_dict_bump 回归：`d[k] = d.get(k, def) + delta` 融合成一次 pa_dict_bump。
#
# 覆盖修过的判据放宽（opt_dict_bump.cpp）：
#   * 循环不变 key（LICM 把 key 装载外提到 preheader，body 里没有 key 装载）；
#   * `+1` 常量在 get 之后才 materialize（get, const, add 顺序）；
#   * 模块级 global dict（LoadGlobal）+ 字面量/变量 key（ConstStr / LoadSlot）；
#   * delta 非 1（+2）、默认值非 0（.get(k, 5)）；
#   * key 每次变化（不会被 LICM 外提的对照形状）。
# 只对拍输出，不断言指令数；-O2 IR 里应出现 pa_dict_bump（见报告）。


def bump_fixed(d: dict, key: str, delta: int, n: int) -> None:
    i = 0
    while i < n:
        d[key] = d.get(key, 0) + delta
        i = i + 1


def bump_default(d: dict, key: str, dflt: int, n: int) -> None:
    i = 0
    while i < n:
        d[key] = d.get(key, dflt) + 1
        i = i + 1


def bump_varying(d: dict, keys: list, n: int) -> None:
    # key 每次从列表取（不触发 LICM 外提 key）——对照形状。
    i = 0
    while i < n:
        k = keys[i % 4]
        d[k] = d.get(k, 0) + 1
        i = i + 1


def assign_only(d: dict, key: str, n: int) -> None:
    # 非「get+add」形态：不融合，只验证输出正确。
    i = 0
    while i < n:
        d[key] = 7
        i = i + 1


keylist = ["k0", "k1", "k2", "k3"]

# 1) 函数内 slot dict，key 固定（LICM 外提），delta = 1
d = {}
bump_fixed(d, "a", 1, 1000)
print("slot_fixed", d["a"])

# 2) 函数内 slot dict，delta = 2
d2 = {}
bump_fixed(d2, "x", 2, 500)
print("slot_delta2", d2["x"])

# 3) 函数内 slot dict，默认值非 0
d3 = {}
bump_default(d3, "y", 5, 500)
print("slot_def5", d3["y"])

# 4) 函数内 slot dict，key 每次变化（对照：body 里有 key 装载）
d4 = {}
bump_varying(d4, keylist, 1000)
print("slot_varying", d4["k0"], d4["k1"], d4["k2"], d4["k3"])

# 5) 模块级 global dict：字面量 key + 常量 delta（LICM 外提 key/常量）
g = {}
i = 0
while i < 1000:
    g["cnt"] = g.get("cnt", 0) + 1
    i = i + 1
print("global_fixed", g["cnt"])

# 6) 模块级 global dict：delta 非 1
h = {}
i = 0
while i < 500:
    h["cnt"] = h.get("cnt", 0) + 2
    i = i + 1
print("global_delta2", h["cnt"])

# 7) 模块级 global dict：key 每次变化（对照）
gm = {}
i = 0
while i < 1000:
    gk = "m" + str(i % 3)
    gm[gk] = gm.get(gk, 0) + 1
    i = i + 1
print("global_varying", gm["m0"], gm["m1"], gm["m2"])

# 8) 非加法形态（不融合）：输出正确
d5 = {}
assign_only(d5, "z", 10)
print("assign_only", d5["z"])

# --- expect ---
# slot_fixed 1000
# slot_delta2 1000
# slot_def5 505
# slot_varying 250 250 250 250
# global_fixed 1000
# global_delta2 1000
# global_varying 334 333 333
# assign_only 7
