"""operator 模块的 pyadamas 子集（纯 pyadamas 源码）。

目前只提供与泛型参数无关、能在静态类型下安全实现的部分：
itemgetter（单键）、truth、not_。attrgetter 需要运行时按名字取属性，
pyadamas 的 getattr 只支持字面量名字，暂未提供；算术与比较函数需要
按调用点特化，也暂未提供。
"""


def itemgetter(item):
    def getter(obj):
        return obj[item]

    return getter


def truth(obj) -> bool:
    return bool(obj)


def not_(obj) -> bool:
    return not bool(obj)
