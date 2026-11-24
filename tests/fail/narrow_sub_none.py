# expect: this value may be None here; check it first
# 下标访问同样要求先判空（0.498）；消息在拿不到名字时用泛化主语。
def f(xs: list[int] | None) -> int:
    return xs[0]