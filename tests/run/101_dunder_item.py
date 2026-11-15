# 用户类的 __getitem__ / __setitem__ 必须按 dunder 分发。
# 以前降级阶段直接落到 IndexLoad/IndexStore，把对象当 PyAdamasList 解引用，
# 运行期直接段错误（既不是 TypeError 也不是正常结果）。
class Box:
    def __init__(self):
        self.d = {}

    def __getitem__(self, k):
        return self.d[k]

    def __setitem__(self, k, v):
        self.d[k] = v


b = Box()
b["a"] = 1
b["b"] = 2
print(b["a"], b["b"])


# 计算式 __getitem__（_QByteMap 那种查表类）
class Doubler:
    def __getitem__(self, k):
        return k * 2


d = Doubler()
print(d[5])


# 继承来的 __setitem__ 也要生效
class Sub(Box):
    pass


s = Sub()
s["x"] = 9
print(s["x"])

# --- expect ---
# 1 2
# 10
# 9
