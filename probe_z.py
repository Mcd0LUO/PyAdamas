def to_int(x: unknown) -> int:
    return x

def ints(ks: list[unknown]) -> list[int]:
    out: list[int] = []
    for k in ks:
        out.append(to_int(k))
    return out

def untyped(ks: list[int]) -> list[unknown]:
    out: list[unknown] = []
    for k in ks:
        out.append(k)
    return out

ks: list[unknown] = [3, 1, 2]
print(ints(ks))
print(sorted(ints(ks)))
print(untyped(sorted(ints(ks))))
