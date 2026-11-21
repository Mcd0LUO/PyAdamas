def add_at_limit(v: big = 18446744073709551615) -> big:
    return v + 1

def offset(*, v: big = -1234567890123456789012345) -> big:
    return v + 4

def hold(v: big) -> object:
    return v

class Cell:
    value: big
    def __init__(self, value: big) -> None:
        self.value = value
    def read(self) -> big:
        return self.value
