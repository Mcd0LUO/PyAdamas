# 265_object_slot_upgrade —— 0.328 P3-1b：object 槽禁止 unknown→typed 升级
#
# 0.327 基线已存在的 RC/类型 bug：store_slot 把 unknown 槽在首次 typed 存储
# 时升级成该类型（int 特化）。显式 object 标注的槽也被升级：s: object = 0
# 升成 int 槽后，s = s + x（unknown）按非堆槽裸存——不 retain、旧值不释放、
# load.slot 按 int 读回：float 盒被 cleanup 提前释放成野指针，指针按整数
# 打印（函数内累加器 return 形态）。修复双管：
#   1) 显式 object/Any 标注的槽禁止升级——按标注 AST 精确判定（局部
#      AnnAssign 的 object/Any + 形参 Param.ann_unknown，含生成器形参）；
#      不用 sym.annotated+unknown（typecheck 对循环内 j: int 也给该组合，
#      误标会改变 d4_primes 等 26 基准 asm）；
#   2) 兜底：unknown/堆值流入已升级的非堆槽时回退 unknown（de-upgrade），
#      走 retain/释放口径（非标注推断槽的 RC 安全网）；
#   3) 配套：生成器 yield 为消费方保留的那份引用，unknown 元素原先无人
#      释放（next/__next__/send/throw/for-gen/list(gen)/genfor 只给堆元素
#      登记临时栈）——补 push_temp_unknown / RcDecUnknown。基线靠「槽升级
#      裸存→盒提前释放→池复用」掩盖成 UAF 链，槽修复后暴露为干净泄漏，
#      一并根治（本用例 gen 段 + ASan detect_leaks=1 钉住）。
# 覆盖：函数内 for/while 累加器 return（直打/槽中转/再算术）、object 形参
# 重赋值、int→str→list 覆写、生成器 object 累加 yield、类字段 object 累加、
# 闭包 object 槽、无标注混合槽。全部先对拍 CPython 再钉 expect；O0/O1/O2 一致。
def suml(xs: list[object]) -> object:
    s: object = 0
    for x in xs:
        s = s + x
    return s
xs: list[object] = [1.5, 2, 3.5]
print(suml(xs))
v: object = suml(xs)
print(v)
print(suml(xs) + 1)
def f(x: object) -> object:
    x = 0
    x = x + 2.5
    return x
print(f(9))
def g(n: int) -> object:
    s: object = 0
    i: int = 0
    while i < n:
        s = s + i
        i = i + 1
    return s
print(g(5))
print(g(0))
def h() -> object:
    s: object = 1
    s = "a"
    s = s + "b"
    return s
print(h())
def lst() -> object:
    s: object = 0
    s = [1, 2]
    return s
tl: object = lst()
print(tl)
def mixed() -> object:
    s: object = 0
    s = 1.5
    s = s + 1
    s = "z"
    return s
print(mixed())
def gen(n: int):
    s: object = 0
    i: int = 0
    while i < n:
        s = s + 0.5
        yield s
        i = i + 1
for gv in gen(3):
    print(gv)
class Acc:
    def __init__(self):
        self.total: object = 0
    def add(self, x: object) -> object:
        self.total = self.total + x
        return self.total
a: Acc = Acc()
print(a.add(1))
print(a.add(2.5))
a2: Acc = Acc()
print(a2.add(10))
print(a2.add(0.5))
def outer(n: int) -> object:
    s: object = 0
    def inner() -> object:
        return s + 1
    i: int = 0
    while i < n:
        s = s + 0.25
        i = i + 1
    return inner()
print(outer(4))
def unann():
    s = 0
    s = s + 0.5
    return s
print(unann())
# --- expect ---
# 7.0
# 7.0
# 8.0
# 2.5
# 10
# 0
# ab
# [1, 2]
# z
# 0.5
# 1.0
# 1.5
# 1
# 3.5
# 10
# 10.5
# 2.0
# 0.5
