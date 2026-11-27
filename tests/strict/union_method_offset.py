# expect: method 'm' is not supported by the code generator yet
# 两个成员都有 m、但字段布局不同：静态选第一个成员仍然错（按 C 的偏移读 D 的
# 实例 → 静默错值/越界）。真·多类联合必须显式报错；要支持只能做运行期 tag 分派。
class C:
    def __init__(self) -> None:
        self.v = 5

    def m(self) -> int:
        return self.v

class D:
    def __init__(self) -> None:
        self.pad = 1
        self.w = 7

    def m(self) -> int:
        return self.w

def f(x: C | D) -> int:
    return x.m()

print(f(C()))
print(f(D()))
