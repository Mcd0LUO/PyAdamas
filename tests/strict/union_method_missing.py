# expect: method 'm' is not supported by the code generator yet
# 联合接收者（C | D）里只有 C 定义了 m：降级层不能静态选 C 直调（D 的实例会
# 按 C 的布局读字段/越界），多类联合一律显式报错。类型检查侧 0.678 同一口径。
class C:
    def __init__(self) -> None:
        self.v = 5

    def m(self) -> int:
        return self.v

class D:
    def __init__(self) -> None:
        self.pad = 1
        self.w = 7

    def n(self) -> int:
        return self.w

def f(x: C | D) -> int:
    return x.m()

print(f(D()))
