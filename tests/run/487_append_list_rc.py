# append 建的 list 传参后按内容/RC 正确的回归测试。
#
# 背景 bug：`{}` 字面量在没有类型上下文时建出 key_kind=3 的 dict（按机器字
# 比指针）；键是「动态构造的 str」（append 建 list 的元素、函数参数、列表下标
# 取出的 str）时，等值的新对象查不到 → KeyError。修复见 runtime/rt_dict.c：
# unknown 键域（kind 3/9）在运行时认出堆字符串后按内容哈希/比较；同时
# ir/ir.cpp 让 key_kind 3/9 的 dict 强制保留精确堆索引（探针需要）。
#
# 覆盖：append 建 list 传参 + 下标读/写；元素 str/int/float；多层调用；
# 函数内修改 list；list 作为 dict 键取值来源；typed/untyped 参数对照。


def sum_list(xs: list, n: int) -> int:
    i = 0
    s = 0
    while i < n:
        s = s + xs[i]
        i = i + 1
    return s


def bump_list(xs: list, n: int) -> None:
    # 函数内按下标写回 list（扩容后指针/长度必须正确）。
    i = 0
    while i < n:
        xs[i] = xs[i] + 1
        i = i + 1


def sum_float(xs: list, n: int) -> float:
    i = 0
    s = 0.0
    while i < n:
        s = s + xs[i]
        i = i + 1
    return s


def count_keys(d: dict, keys: list, n: int) -> None:
    # 复现 bug 的原形状：untyped dict + append 建的 list + str 键。
    i = 0
    while i < n:
        k = keys[i % 4]
        d[k] = d.get(k, 0) + 1
        i = i + 1


def lookup2(d: dict, k1: str, k2: str) -> int:
    # 多层调用 + 动态构造的等值 str 键查表。
    return d[k1] + d[k2]


def append_inside(n: int) -> list:
    # 函数内 append 建 list 再返回，元素是动态 str。
    xs = []
    i = 0
    while i < n:
        xs.append("v" + str(i))
        i = i + 1
    return xs


# 1) append 建的 int list，传参求和 + 函数内写回
ints = []
for j in range(1000):
    ints.append(j)
print("int_sum", sum_list(ints, 1000))
bump_list(ints, 1000)
print("int_bumped", ints[0], ints[999], sum_list(ints, 1000))

# 2) append 建的 float list，传参求和
floats = []
for j in range(100):
    floats.append(float(j) + 0.5)
print("float_sum", sum_float(floats, 100))

# 3) append 建的 str list 作为 dict 键来源（原 bug 形状）
keys = []
for j in range(4):
    keys.append("k" + str(j))
d = {}
count_keys(d, keys, 1000)
print("str_keys", d["k0"], d["k1"], d["k2"], d["k3"])

# 4) 多层调用：动态构造新 str 键查同一张表
print("lookup2", lookup2(d, "k" + str(2), "k" + str(3)))

# 5) 函数内 append 再返回的 list：传参 + 下标读 + 查表
xs = append_inside(1000)
seen = {}
i = 0
while i < 1000:
    k = xs[i]
    seen[k] = seen.get(k, 0) + 1
    i = i + 1
print("append_inside", len(xs), xs[0], xs[999], seen["v0"], seen["v999"])

# 6) 对照：typed list[str] 参数（修复前也能过，但必须有回归保护）
typed_keys = []
for j in range(4):
    typed_keys.append("t" + str(j))


def count_typed(d: dict, keys: list[str], n: int) -> None:
    i = 0
    while i < n:
        k = keys[i % 4]
        d[k] = d.get(k, 0) + 1
        i = i + 1


d2 = {}
count_typed(d2, typed_keys, 1000)
print("typed_keys", d2["t0"], d2["t1"], d2["t2"], d2["t3"])

# 7) 对照：int 键 dict（不涉及堆字符串，修复不能破坏）
d3 = {}
ints2 = []
for j in range(4):
    ints2.append(j * 2)
i = 0
while i < 100:
    k = ints2[i % 4]
    d3[k] = d3.get(k, 0) + 1
    i = i + 1
print("int_keys", d3[0], d3[2], d3[4], d3[6])

# 8) 函数内修改 list 后调用方能看到（传引用语义）

def push(xs: list, v: int) -> None:
    xs.append(v)


lst = [1, 2, 3]
push(lst, 4)
push(lst, 5)
print("push", len(lst), lst[3], lst[4])

# --- expect ---
# int_sum 499500
# int_bumped 1 1000 500500
# float_sum 5000.0
# str_keys 250 250 250 250
# lookup2 500
# append_inside 1000 v0 v999 1 1
# typed_keys 250 250 250 250
# int_keys 25 25 25 25
# push 5 4 5
