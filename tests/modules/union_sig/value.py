class Value:
    def __init__(self, v: float) -> None:
        self.v = v
    def get(self) -> float:
        return self.v
    def add(self, o: "Value | float") -> "Value":
        if isinstance(o, Value):
            return Value(self.v + o.v)
        return Value(self.v + float(o))
