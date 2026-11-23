"""copy 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

**语义边界（重要）**：PyAdamas 是静态类型编译器，`isinstance` 不能把一个 unknown
形参收窄成具体容器类型，所以 copy / deepcopy 没法写成「一个函数处理所有容器」。
这里提供的是**按容器类型分开的显式入口**：

    copy_list(xs) / copy_dict(d) / copy_set(s) / deepcopy_list(xs) / deepcopy_dict(d)

以及一个 `copy(x)`：它按静态类型分派（调用点的实参类型已知时能正确选择）。
对 unknown 实参，`copy` 返回原值 —— 调用方应当改用上面的具体入口。

与 CPython 的差异：没有 `__copy__` / `__deepcopy__` 协议（运行期动态协议，本项目没有），
也不递归类实例字段。
"""


def copy_list(xs: list) -> list:
    """list 的浅拷贝。"""
    return xs[:]


def copy_dict(d: dict) -> dict:
    """dict 的浅拷贝。"""
    return d.copy()


def copy_set(s: set) -> set:
    """set 的浅拷贝。"""
    return set(s)


def deepcopy_list(xs: list) -> list:
    """list 的深拷贝（逐层复制嵌套的 list / dict / set）。"""
    out: list = []
    i: int = 0
    while i < len(xs):
        out.append(deepcopy_any(xs[i]))
        i += 1
    return out


def deepcopy_dict(d: dict) -> dict:
    """dict 的深拷贝。"""
    out: dict = {}
    for k in d:
        out[k] = deepcopy_any(d[k])
    return out


def deepcopy_any(x):
    """按运行期类型递归复制。unknown 实参按原值返回（无法判定类型）。"""
    if isinstance(x, list):
        return deepcopy_list(x)
    if isinstance(x, dict):
        return deepcopy_dict(x)
    return x


def copy(x):
    """浅拷贝入口：按运行期类型分派（list / dict / set）。

    与 CPython copy.copy 对齐：容器返回新对象（共享元素引用），
    标量/未知类型原值返回。
    """
    if isinstance(x, list):
        return copy_list(x)
    if isinstance(x, dict):
        return copy_dict(x)
    if isinstance(x, set):
        return copy_set(x)
    return x


def deepcopy(x):
    """深拷贝入口：按运行期类型递归复制（list/dict/set）。"""
    return deepcopy_any(x)
