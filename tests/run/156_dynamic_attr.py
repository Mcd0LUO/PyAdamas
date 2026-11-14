# 实例动态属性：静态布局里没有的成员，setattr 后能 getattr / 直接读 / hasattr。
# 0.165 起类实例末尾可挂一个惰性分配的 dict[str, unknown]。
class Counter:
    def __init__(self):
        self.n = 0


c = Counter()
setattr(c, "label", "L1")
setattr(c, "label", "L2")
print(c.label)
print(getattr(c, "label", "no"))
print(getattr(c, "missing", "no"))
print(hasattr(c, "label"), hasattr(c, "missing"))
key = "extra"
setattr(c, key, 7)
print(c.extra, hasattr(c, key))
b = Counter()
print(hasattr(b, "label"))

# --- expect ---
# L2
# L2
# no
# True False
# 7 True
# False
