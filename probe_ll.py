def g1():
    yield 1, 2

def g2():
    try:
        yield 3
    finally:
        print("cleanup")

it1 = g1()
v = next(it1)
print(v[0], v[1])
for _ in it1:
    pass
it2 = g2()
print(next(it2))
for _ in it2:
    pass
