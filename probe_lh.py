def g2():
    yield 1

it = g2()
print(next(it))
for _ in it:
    pass
