# 普通虚调用：同一组调用点接收基类、继承实现和不同的同槽覆盖。
from typing import Any


class Reading:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def tag(self) -> int:
        return 11

    def value(self) -> int:
        return self.x

    def product(self) -> int:
        return self.x * self.y


class SwappedReading(Reading):
    def tag(self) -> int:
        return 29

    def value(self) -> int:
        return self.y

    def product(self) -> int:
        return self.y * self.y


class InheritedReading(SwappedReading):
    pass


class CountingReading(Reading):
    def tag(self) -> int:
        return 47

    def value(self) -> int:
        self.x += 1
        return self.x


class BrokenReading(Reading):
    def tag(self) -> int:
        self.x += 1
        return 59

    def product(self) -> int:
        self.x += 10
        raise ValueError("broken product")


class UnrelatedReading:
    # 和 Reading 的方法同槽，但不是其子类，且字段布局不同。
    def __init__(self, x: int, y: int) -> None:
        self.y = y
        self.x = x

    def tag(self) -> int:
        return 71

    def value(self) -> int:
        return self.x - self.y

    def product(self) -> int:
        return self.x * self.y + 5


def probe(reading: Reading) -> None:
    tag = reading.tag()
    # value 的回退必须执行恰好一次，且随后的 product 要读到新值。
    value = reading.value()
    product = reading.product()
    print(tag, value, product)


# Any 允许同槽的非子类通过静态类型为 Reading 的调用点。
unrelated: Any = UnrelatedReading(8, 9)
counting = CountingReading(10, 7)
readings: list[Reading] = [
    Reading(3, 5),
    SwappedReading(4, 6),
    InheritedReading(6, 8),
    unrelated,
    counting,
    counting,
    BrokenReading(9, 2),
    Reading(-2, 4),
]
for reading in readings:
    try:
        probe(reading)
    except ValueError as err:
        # 抛错前的写入必须保留；probe 中的 print 则不能执行。
        print("caught", err, reading.x)

# --- expect ---
# 11 3 15
# 29 6 36
# 29 8 64
# 71 -1 77
# 47 11 77
# 47 12 84
# caught broken product 20
# 11 -2 -8
