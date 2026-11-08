# 类型层：推断、提升、收窄、容器、方法表
#
# 固定住这一轮修掉的具体缺陷：
#   * *args 是 tuple 而不是 list（(v,) + args 合法）
#   * 元组切片长度不定，`a, b = t[-2:]` 不该报「不能拆成 2 个」
#   * tuple 可以按字典序比较
#   * `x = None` 只是占位，不能把槽位收窄成 None
#   * try 里改过的变量，except 里按公共上界看
#   * int.bit_length / float.numerator 等成员存在

from typing import Optional


def numeric(a: int, b: float) -> float:
    # int 与 float 混合运算按提升规则得到 float
    return a + b


def promoted() -> float:
    total = 0
    for v in [1, 2, 3]:
        total = total + v
    return total / 2


def optional_param(value: Optional[int]) -> int:
    # is not None 之后按 int 用
    if value is not None:
        return value + 1
    return 0


def early_exit(cache: Optional[dict[str, int]]) -> int:
    # 提前退出守卫：if ... is None: return 之后，cache 不是 None
    if cache is None:
        return 0
    return len(cache)


def lazy_init(flag: bool) -> str:
    # 先置 None 再分支赋值：不能因为占位就当成 None
    label = None
    if flag:
        label = "on"
    else:
        label = "off"
    return label


def try_rollback(text: str) -> int:
    # try 里 v 被改成 int，except 里它仍然是 str
    try:
        v = int(text)
    except ValueError:
        return len(text)
    return v


def variadic(first: int, *rest) -> tuple[int, ...]:
    # *rest 是 tuple：可以拼接、可以解包
    combined = (first,) + rest
    total = 0
    for v in combined:
        total = total + v
    return combined


def tuple_slice(pair: tuple[int, int, str]) -> None:
    # 切片结果长度不定，两个目标都能解出来
    a, b = pair[-2:]
    first, *others = pair
    print(a, b, first, others)


def tuple_order(a: tuple[int, int], b: tuple[int, int]) -> bool:
    # 版本号/日期这类元组按字典序比较
    return a < b


def int_members(n: int, f: float) -> int:
    # 数值类型的成员表
    bits = n.bit_length()
    num = n.numerator
    den = n.denominator
    real = f.real
    print(num, den, real)
    if f.is_integer():
        return bits + 1
    return bits


def set_ops(a: set[int], b: set[int]) -> set[int]:
    merged = a | b
    merged.update(b)
    merged.difference_update(b)
    merged.symmetric_difference(b)
    if a.issubset(b) and a.isdisjoint(b):
        return merged
    return merged


def container_inference() -> None:
    # 空容器先占位，后续插入再定元素类型
    xs = []
    xs.append(1)
    xs.append(2)

    mapping = {}
    mapping["k"] = 1

    # 异构字面量取公共上界，不因为「都是 int」以外的原因报错
    mixed = [1, 2.5]
    print(xs, mapping, mixed)


def shadow_and_rebind() -> None:
    # 同一名字先当数字、后当字符串：Python 合法，取公共上界后不再检查
    value = 1
    print(value)
    value = "s"
    print(value)


def annotated_local() -> int:
    count: int = 0
    count = count + 1
    return count
