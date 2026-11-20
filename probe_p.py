class R:
    scheme: str
    def __init__(self, s: str) -> None:
        self.scheme = s

def getfield(x: unknown) -> unknown:
    return getattr(x, "scheme")

def first(x: unknown) -> str:
    out = ""
    for v in x:
        out = out + v
        break
    return out

r = R("http")
print(getfield(r))
print(first((1, 2, 3)))
print(first([4, 5]))
