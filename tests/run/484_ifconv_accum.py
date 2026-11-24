# if-conversion：简单菱形（then 块 = 累加器自增/自减 ±1、else 空）压成单块。
#
# 判据（见 src/opt/ifconv.cpp）：
#   * then 块只有「不抛异常、无副作用、不读可能非法指针」的指令；
#     store 唯一且是最后一条；累加器必须是同一个**局部槽**；常量 k 必须 ±1；
#   * then 块唯一前驱是条件块；条件值是 bool（机器上 0/1）；
#   * 函数带异常 handler / 生成器 / EH 标记时整段不动。
# 不满足的（条件赋值、有 else、嵌套 if、then 块里带下标装载、全局累加器）
# 一律保守保持原分支 —— 本用例同时验证这些形状输出仍正确。


def acc_if(n: int) -> int:
    s = 0
    i = 0
    while i < n:
        if i % 2 == 0:
            s = s + 1
        i = i + 1
    return s


def acc_if_minus(n: int) -> int:
    s = 100
    i = 0
    while i < n:
        if i % 3 == 0:
            s = s - 1
        i = i + 1
    return s


def assign_if(n: int) -> int:
    # 条件赋值（不是累加）→ 保持分支。
    s = 0
    i = 0
    while i < n:
        if i % 2 == 0:
            s = i
        i = i + 1
    return s


def else_if(n: int) -> int:
    # 有 else → 最小可用版不动（保持分支）。
    s = 0
    i = 0
    while i < n:
        if i % 2 == 0:
            s = s + 1
        else:
            s = s + 2
        i = i + 1
    return s


def nested_if(n: int) -> int:
    # 嵌套 if：then 块里有分支 → 不动。
    s = 0
    i = 0
    while i < n:
        if i % 2 == 0:
            if i % 4 == 0:
                s = s + 1
        i = i + 1
    return s


def load_expr(n: int) -> int:
    # 下标装载在**条件块**里（本来每次迭代都无条件执行）→ then 块是纯累加，可安全转换。
    v = []
    for k in range(n):
        v.append(k % 5)
    s = 0
    i = 0
    while i < n:
        if v[i] == 0:
            s = s + 1
        i = i + 1
    return s


def load_in_then(n: int) -> int:
    # then 块的累加表达式带下标装载（IndexLoad 可能抛 IndexError）→ 保守不动。
    v = []
    for k in range(n):
        v.append(k % 5)
    s = 0
    i = 0
    while i < n:
        if i % 2 == 0:
            s = s + v[i]
        i = i + 1
    return s


def global_acc(n: int) -> int:
    # 全局累加器：最小可用版不处理（分支化会把全局读写变成每轮无条件执行）。
    global counter
    counter = 0
    i = 0
    while i < n:
        if i % 2 == 1:
            counter = counter + 1
        i = i + 1
    return counter


counter = 0
print("acc_if", acc_if(100))
print("acc_if_minus", acc_if_minus(100))
print("assign_if", assign_if(100))
print("else_if", else_if(100))
print("nested_if", nested_if(100))
print("load_expr", load_expr(100))
print("load_in_then", load_in_then(100))
print("global_acc", global_acc(100))

# --- expect ---
# acc_if 50
# acc_if_minus 66
# assign_if 98
# else_if 150
# nested_if 25
# load_expr 20
# load_in_then 100
# global_acc 50
