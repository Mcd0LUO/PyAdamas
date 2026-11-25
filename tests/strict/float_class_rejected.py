# 反例（P0-C）：类实例 → float 必须编译期报错（过去是指针位模式直通）。
class Value:
    def __init__(self, v: float) -> None:
        self.v = v

x: float = float(Value(3.0))
