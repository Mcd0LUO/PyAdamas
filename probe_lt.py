def g1():
    yield 1, 2

def g2():
    try:
        yield 7
    finally:
        print("cleanup")

it1 = g1()
it2 = g2()
v1 = next(it1)
v2 = next(it2)
print(v1[0], v2)
for _ in it1:
    pass
for _ in it2:
    pass
