# expect: 'x' may be None here; check it first
# 0.498：`T | None` 没判空就做算术 → 编译期报错，消息给可操作提示。
# 窄化只影响静态类型，不改变运行期表示（联合仍是 8 字节字 + 成员表）。
def f(x: int | None) -> int:
    return x + 1