# isinstance 对动态值的判定（0.528）：静态类型是 unknown/联合时必须查运行期类型标签。
# 修前（类型标签默认关）isinstance(x, int) 对动态值恒为 False —— 静默错值：
#   h(1) -> 0（应为 1）、g([1, "a", 2.0, 3]) -> 0（应为 2）。
# 现在按需开启标签（源码出现 isinstance 时），12 基准不含 isinstance、不受影响。


def h(o: object) -> int:
    return 1 if isinstance(o, int) else 0


def g(xs: list) -> int:
    n = 0
    for x in xs:
        if isinstance(x, int):
            n = n + 1
    return n


def kind(o: object) -> str:
    if isinstance(o, str):
        return "str"
    if isinstance(o, int):
        return "int"
    if isinstance(o, float):
        return "float"
    if isinstance(o, list):
        return "list"
    return "other"


print(h(1), h("x"), h(None), h(0))
print(g([1, "a", 2.0, 3]))
print(kind(1), kind("s"), kind(1.5), kind(True), kind([1]), kind(None))

# --- expect ---
# 1 0 0 1
# 2
# int str float int list other
