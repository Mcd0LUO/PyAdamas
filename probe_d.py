def show(x: unknown) -> None:
    print(str(x))
    if isinstance(x, str):
        print("str", len(x))
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
    elif isinstance(x, bool):
        print("bool")
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

def varfun(*args) -> int:
    return len(args)

print(varfun(1, 2, 3))

def callit(f: unknown, *args) -> unknown:
    return f(*args)

def add3(a: int, b: int, c: int) -> int:
    return a + b + c

print(callit(add3, 1, 2, 3))

def kwfun(**kw) -> int:
    return len(kw)

print(kwfun(a=1, b=2))

for k in {"x": 1, "y": 2}:
    print("key", k)
d = {"x": 1}
print(d.items())
