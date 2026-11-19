# 264_p3_dict_field_tag —— 0.328 P3-1b 步骤2：dict 值/字段装载一次探针导 tag
#
# dict 值与类字段没有平行 tag 来源（容器/对象布局不变，仍 8B 槽）。门控内
# 装载 unknown 值时发一次 pa_any_load_tag（探针+kind），后续 RC 三入口/
# 动态算子按 tag 免探针 —— 比「每次使用都探针」少（设计 §3.3 取出侧）。
# 覆盖：DictGet/DictGetOr（含 miss 默认值）/DictPop、字段装载+覆写后旧值
# tag 保持、DictTryGet 融合（if k in d: v=d[k]，miss 分支）、dict 值过直调
# 形参/返回（与步骤1 串联）、模块级 dict 累加循环。
# 注：嵌套 list[unknown] 的 print repr 与「函数内 object 槽升级 int 后收
# unknown」两形态是 0.327 基线已存在的独立 bug（基线/本分支输出逐字相同），
# 不在本用例范围；后者在 265 修复，前者记录移交。
# 全部先对拍 CPython 再钉 expect；O0/O1/O2 输出一致。
class Box:
    def __init__(self, v: object):
        self.v: object = v
d: dict[str, object] = {"a": 1.5, "b": 2, "c": "s", "d": 3.5}
acc: object = 0
i: int = 0
keys: list[str] = ["a", "b", "c", "d"]
while i < 8:
    k: str = keys[i % 4]
    v: object = d[k]
    if k != "c":
        acc = acc + v
    i = i + 1
print(acc)
print(d["a"] + d["d"])
print(d.get("b", 0))
print(d.get("zz", 9.5))
print(d.pop("d"))
print(len(d))
b: Box = Box(2.5)
print(b.v + 1)
w: object = b.v
b.v = 10
print(w + 0.5)
print(b.v)
b.v = "t"
print(b.v + "!")
if "a" in d:
    fv: object = d["a"]
    print(fv * 2)
if "zz" in d:
    print(d["zz"])
else:
    print("miss")
tot: object = 0
j: int = 0
while j < 4:
    tot = tot + d.get("a", 0)
    j = j + 1
print(tot)
def getv(dd: dict[str, object], kk: str) -> object:
    return dd[kk]
print(getv(d, "a") + 1)
print(getv(d, "b") * 3)
sv: object = getv(d, "a")
print(sv)
# --- expect ---
# 14.0
# 5.0
# 2
# 9.5
# 3.5
# 3
# 3.5
# 3.0
# 10
# t!
# 3.0
# miss
# 6.0
# 2.5
# 6
# 1.5
