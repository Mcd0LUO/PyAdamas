# 返回类型未知的方法装饰器：装饰器链结果按 unknown 调用。

def identity(f):
    return f


def opaque(f):
    box: list[object] = [f]
    return box[0]


class C:
    @identity
    def known(self, x: int) -> int:
        return x + 1

    @opaque
    def unknown(self, x: int) -> int:
        return x + 2


c = C()
print(c.known(1))
print(c.unknown(2))
print(C.unknown(c, 3))

# --- expect ---
# 2
# 4
# 5
