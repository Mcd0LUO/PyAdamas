class Box:
    value: int
    def __init__(self, value: int) -> None:
        self.value = value


class Holder:
    b: Box
    def __init__(self, b: Box) -> None:
        self.b = b
