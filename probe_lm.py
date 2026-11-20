def g():
    yield 1, 2

def callit(f: unknown) -> Generator[unknown]:
    return f()

it = callit(g)
v = next(it)
print(v[0], v[1])
for _ in it:
    pass
