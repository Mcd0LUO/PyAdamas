class R:
    scheme: str
    netloc: str
    path: str

    def __init__(self, scheme: str, netloc: str, path: str) -> None:
        self.scheme = scheme
        self.netloc = netloc
        self.path = path

    def __repr__(self) -> str:
        return "R(scheme=" + repr(self.scheme) + ", netloc=" + repr(self.netloc) + ")"

    def __len__(self) -> int:
        return 3

    def __getitem__(self, i: int) -> str:
        if i == 0:
            return self.scheme
        if i == 1:
            return self.netloc
        return self.path

r = R("http", "host", "/p")
print(r)
print(repr(r))
print(len(r), r[0], r[2])
a, b, c = r
print(a, b, c)
print(r == R("http", "host", "/p"))
