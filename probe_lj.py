def g():
    yield [1, 2]

it = g()
print(next(it)[0])
for _ in it:
    pass
