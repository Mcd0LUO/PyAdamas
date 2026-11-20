class CM:
    func: unknown
    args: unknown
    gen: unknown

    def __init__(self, func: unknown, args: unknown) -> None:
        self.func = func
        self.args = args
        self.gen = None

    def __enter__(self) -> unknown:
        if len(self.args) == 0:
            self.gen = self.func()
        elif len(self.args) == 1:
            self.gen = self.func(self.args[0])
        else:
            self.gen = self.func(self.args[0], self.args[1])
        return next(self.gen)

    def __exit__(self, et, ev, tb) -> bool:
        try:
            next(self.gen)
        except StopIteration:
            return False
        return False

def contextmanager(func: unknown) -> unknown:
    def helper(*args) -> CM:
        return CM(func, args)
    return helper

@contextmanager
def tag(name: str):
    print("start", name)
    yield name
    print("end", name)

with tag("x") as v:
    print("body", v)

@contextmanager
def two(a: int, b: int):
    yield a + b

with two(1, 2) as s:
    print("sum", s)

def show(x: unknown) -> None:
    print(str(x))
    if isinstance(x, str):
        print("str", len(x))
    elif isinstance(x, bool):
        print("bool")
    elif isinstance(x, int):
        print("int")
    elif isinstance(x, float):
        print("float")
    elif isinstance(x, list):
        print("list", len(x))
    elif isinstance(x, dict):
        print("dict", len(x))
    elif isinstance(x, tuple):
        print("tuple", len(x))
    else:
        print("other")

show(3)
show(3.5)
show(True)
show("ab")
show([1, "x"])
show({"a": 1})
show((1, 2))
show(None)
