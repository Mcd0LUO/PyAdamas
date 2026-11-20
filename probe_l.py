def g():
    try:
        yield 1
    except ValueError as e:
        print("caught", e)
        yield 2

def throwit(gen: Generator[unknown], e: unknown) -> bool:
    try:
        gen.throw(e)
    except StopIteration:
        return True
    return False

it = g()
print(next(it))
print(throwit(it, ValueError("x")))

def g2():
    try:
        yield 1
    except ValueError as e:
        print("caught2", e)

it2 = g2()
print(next(it2))
try:
    it2.throw(ValueError("y"))
except StopIteration:
    print("stop2")

def etype(e: unknown) -> str:
    if e is None:
        return "none"
    return str(e)

print(etype(None), etype(ValueError("z")))
