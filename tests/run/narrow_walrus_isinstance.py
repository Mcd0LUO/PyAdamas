# 窄化的三类假阳性（0.499 的 T | None 窄化）：walrus 目标、isinstance 分支、
# 模块级「先定义后初始化」。628 文件语料里 graphlib.py / pyclbr.py 因此被误拒。


class Base:
    def name(self) -> str:
        return "base"


class Sub(Base):
    def extra(self) -> int:
        return 7


def walrus(d: dict[str, int]) -> int:
    # walrus 参与 is not None：被赋名的 n 也要窄化。
    if (n := d.get("k")) is not None:
        return n + 1
    return 0


def early(d: dict[str, int]) -> int:
    # walrus + 提前返回：if 之后的 m 不是 None。
    if (m := d.get("k")) is None:
        return -1
    return m + 2


def isinst(x: Base | Sub) -> str:
    # isinstance 真值分支窄化成 Sub，else 分支保留 Base。
    if isinstance(x, Sub):
        return x.name() + str(x.extra())
    return x.name()


def opt(x: Base | None) -> str:
    if isinstance(x, Sub):
        return "sub"
    if x is None:
        return "none"
    return x.name()


print(walrus({"k": 1}), walrus({}))
print(early({"k": 1}), early({}))
print(isinst(Sub()), isinst(Base()))
print(opt(Sub()), opt(Base()), opt(None))

# --- expect ---
# 2 0
# 3 -1
# base7 base
# sub base none
