# 普通 VCall 的 SysV ABI：占用非零虚表槽，self 占用第一个整数寄存器。
# 其余 7 个 int 和 10 个 float 交错传入，两类参数各有 2 个落栈。
class Meter:
    def __init__(self, bias: int) -> None:
        self.bias = bias

    def tag(self) -> int:
        return self.bias

    def measure(
        self, a: int, p: float, b: int, q: float, c: int, r: float,
        d: int, s: float, e: int, t: float, f: int, u: float,
        v: float, w: float, x: float, g: int, y: float,
    ) -> float:
        ints = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g
        floats = p + 2.0 * q + 3.0 * r + 4.0 * s + 5.0 * t
        floats += 6.0 * u + 7.0 * v + 8.0 * w + 9.0 * x + 10.0 * y
        return self.bias + ints + floats


class InheritedMeter(Meter):
    pass


class OverrideMeter(Meter):
    def measure(
        self, a: int, p: float, b: int, q: float, c: int, r: float,
        d: int, s: float, e: int, t: float, f: int, u: float,
        v: float, w: float, x: float, g: int, y: float,
    ) -> float:
        if g < 0:
            raise ValueError("negative g")
        return self.bias - 2 * f + 3 * g + 4.0 * x - 5.0 * y


def invoke(meter: Meter, g: int) -> float:
    return meter.measure(
        1, 0.25, 2, 0.5, 3, 0.75, 4, 1.0, 5, 1.25, 6, 1.5,
        1.75, 2.0, 2.25, g, 2.5,
    )


base = Meter(10)
inherited = InheritedMeter(20)
changed = OverrideMeter(30)
print(invoke(base, 7), invoke(inherited, 7), invoke(changed, 7))
try:
    print(invoke(changed, -7))
except ValueError as err:
    print("caught", err)
print(invoke(changed, 8))

# --- expect ---
# 246.25 256.25 35.5
# caught negative g
# 38.5
