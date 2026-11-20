def gen(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

def resume2(g: Generator[unknown]) -> bool:
    done = False
    try:
        next(g)
    except StopIteration:
        done = True
    return done

it = gen(1)
print(next(it))
print(resume2(it))
