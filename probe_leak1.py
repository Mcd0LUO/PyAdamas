def gen(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

it = gen(1)
print(next(it))
try:
    next(it)
except StopIteration:
    print("done")

try:
    raise ValueError("x")
except ValueError:
    print("caught")
