# 反例（P0-C）：类实例 → int 必须编译期报错。
class Value:
    def __init__(self, v: float) -> None:
        self.v = v

print(int(Value(3.0)))
