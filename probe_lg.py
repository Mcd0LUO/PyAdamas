def g():
    try:
        yield 1
    finally:
        print("finally")

it = g()
print(next(it))
for _ in it:
    pass
