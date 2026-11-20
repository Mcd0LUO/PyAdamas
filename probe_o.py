class R:
    scheme: str
    def __init__(self, s: str) -> None:
        self.scheme = s

def getfield(x: unknown) -> str:
    return x.scheme

def getitem(x: unknown) -> unknown:
    return x[0]

def is_tup(x: unknown) -> bool:
    return isinstance(x, tuple)

r = R("http")
print(getfield(r))
print(getitem([1, 2]))
print(is_tup((1, 2)), is_tup([1]))
print(is_tup(r))
