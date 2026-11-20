def gen(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

def resume(g: Generator[unknown]) -> bool:
    try:
        next(g)
    except StopIteration:
        return True
    return False

it = gen(1)
print(next(it))
print(resume(it))
