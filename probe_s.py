def g(d: dict[str, str]) -> str:
    out = ""
    for k in d:
        out = out + k + "=" + d[k] + "&"
    return out

def p(lst: list[tuple[str, str]]) -> str:
    out = ""
    for it in lst:
        out = out + it[0] + "=" + it[1] + "&"
    return out

def enc(q: unknown) -> str:
    if isinstance(q, dict):
        return g(q)
    return p(q)

print(enc({"a": "1", "b": "2"}))
print(enc([("a", "1")]))

def gen_enter(x: unknown) -> unknown:
    return _next(x)

def _next(g: Generator[unknown]) -> unknown:
    return next(g)

def make_gen(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

gg = make_gen(3)
print(_next(gg))
print(gen_enter(gg))
