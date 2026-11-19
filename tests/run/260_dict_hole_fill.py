# 空容器字面量类型洞填充（0.326）：字段初始化/重赋值、dict 下标存空容器、
# append({})、return {}、f({}) 都按目标类型建表——动态 str 键内容哈希，
# 成员查询/下标读回正确（修复前 unknown 种类建表，指针哈希查不回）。
class Store:
    sections: dict[str, dict[str, str]]
    counts: dict[str, int]
    items: list[dict[str, int]]

    def __init__(self) -> None:
        self.sections = {}
        self.counts = {}
        self.items = []

    def add(self, name: str, key: str, val: str) -> None:
        if name not in self.sections:
            empty: dict[str, str] = {}
            self.sections[name] = empty
        self.sections[name][key] = val
        self.counts[name] = 1
        item: dict[str, int] = {}
        self.items.append(item)
        self.items[len(self.items) - 1][key] = len(val)

    def reassign(self) -> None:
        self.counts = {}
        k = "dyn" + "amic"
        self.counts[k] = 42


s = Store()
s.add("sec", "key", "Value")
s.add("sec", "k2", "v2")
s.add("other", "x", "y")
print("sec" in s.sections, "other" in s.sections, "nope" in s.sections)
print(s.sections["sec"]["key"], s.sections["sec"]["k2"], s.sections["other"]["x"])
print(s.counts["sec"], s.counts["other"])
print(s.items[0]["key"], s.items[1]["k2"], s.items[2]["x"])
s.reassign()
print("dynamic" in s.counts, s.counts["dynamic"])
print(len(s.counts))


def make() -> dict[str, int]:
    return {}


m = make()
m["a" + "b"] = 7
print("ab" in m, m["ab"])


def take(d: dict[str, int]) -> int:
    d["k" + "ey"] = 3
    return d["key"]


print(take({}))


def makelist() -> list[dict[str, str]]:
    return [{}]


ml = makelist()
ml[0]["z"] = "q"
print(ml[0]["z"])

# 局部重赋值与嵌套
loc: dict[str, list[str]] = {}
kk = "a" + "b"
loc[kk] = ["v"]
loc = {}
loc[kk] = ["w"]
print(kk in loc, loc[kk][0])

# int 键
di: dict[int, str] = {}
base = 10
di[base + 1] = "eleven"
print(11 in di, di[11])

# set 空字面量按标注
st: set[str] = set()
st.add("d" + "yn")
st.add("dyn")
print(len(st))

# 异构列表不得被单态化：[1, "x"] 传给未标注形参后仍按混合种类建表
def seth(l, i, v: str):
    l[i] = v


srt = [1, "x"]
seth(srt, 0, "c")
print(srt[0], srt[1])
seth(srt, 1, "a")
print(srt[0], srt[1])
# --- expect ---
# True True False
# Value v2 y
# 1 1
# 5 2 1
# True 42
# 1
# True 7
# 3
# q
# True w
# True eleven
# 1
# c x
# c a
