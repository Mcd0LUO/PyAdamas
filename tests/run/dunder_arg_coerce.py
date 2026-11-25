# dunder 实参 coerce（P0-A）：`q + 2.0` / `q + 2` / `q == 6.0` 过去会静默算错
# （VCall 按实参静态类型分类 ABI，形参是 union 时读到 double 位模式）。
class Q:
    def __init__(self, x: float) -> None:
        self.x = x
    def __add__(self, o: "Q | float") -> "Q":
        if isinstance(o, Q):
            return Q(self.x + o.x)
        return Q(self.x + float(o))
    def __sub__(self, o: "Q | float") -> "Q":
        if isinstance(o, Q):
            return Q(self.x - o.x)
        return Q(self.x - float(o))
    def __mul__(self, o: "Q | float") -> "Q":
        if isinstance(o, Q):
            return Q(self.x * o.x)
        return Q(self.x * float(o))
    def __truediv__(self, o: "Q | float") -> "Q":
        if isinstance(o, Q):
            return Q(self.x / o.x)
        return Q(self.x / float(o))
    def __pow__(self, o: "Q | float") -> "Q":
        if isinstance(o, Q):
            return Q(self.x ** o.x)
        return Q(self.x ** float(o))
    def __eq__(self, o: "Q | float") -> bool:
        if isinstance(o, Q):
            return self.x == o.x
        return self.x == float(o)
    def __ne__(self, o: "Q | float") -> bool:
        if isinstance(o, Q):
            return self.x != o.x
        return self.x != float(o)
    def __lt__(self, o: "Q | float") -> bool:
        if isinstance(o, Q):
            return self.x < o.x
        return self.x < float(o)
    def __contains__(self, o: "Q | float") -> bool:
        if isinstance(o, Q):
            return self.x == o.x
        return self.x == float(o)

q: Q = Q(6.0)
print((q + 2.0).x, (q + 2).x, (q == 6.0))
print((q - 2.0).x, (q - 2).x, (q != 6.0))
print((q * 2.0).x, (q * 2).x)
print((q / 2.0).x, (q / 2).x)
print((q ** 2.0).x, (q ** 2).x)
print(q < 7.0, q < 5.0, q == Q(6.0), q != Q(6.0))
print(6.0 in q, 7.0 in q)

class S:
    def __init__(self, s: str) -> None:
        self.s = s
    def __add__(self, o: str) -> "S":
        return S(self.s + o)

print((S('ab') + 'cd').s)

# --- expect ---
# 8.0 8.0 True
# 4.0 4.0 False
# 12.0 12.0
# 3.0 3.0
# 36.0 36.0
# True False True False
# True False
# abcd
