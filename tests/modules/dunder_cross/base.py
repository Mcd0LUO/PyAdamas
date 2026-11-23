class Base:
    def __init__(self, v: int) -> None:
        self.v = v

    def __eq__(self, other: "Base") -> bool:
        return self.v == other.v

    def __lt__(self, other: "Base") -> bool:
        return self.v < other.v


def make(v: int) -> Base:
    return Base(v)
