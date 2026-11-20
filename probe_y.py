def keytext(k: unknown) -> str:
    return str(k)

def lookup(d: dict[unknown, unknown], k: unknown) -> unknown:
    return d[k]

def srt(d: dict[unknown, unknown]) -> str:
    ks: list[unknown] = []
    for k in d:
        ks.append(k)
    ks = sorted(ks, key=keytext)
    out = ""
    for k in ks:
        out = out + str(k) + "=" + str(lookup(d, k)) + ","
    return out

print(srt({"b": 2, "a": 1, 10: "x"}))

def st(x: set) -> str:
    vals: list[unknown] = []
    for v in x:
        vals.append(v)
    vals = sorted(vals, key=keytext)
    out = ""
    for v in vals:
        out = out + str(v) + ","
    return out
print(st({3, 1, 2}))
print(repr(1.5), repr(-0.0), repr(1e300))
print(repr("a'b"), repr("a\"b"))

def show(d: unknown) -> None:
    for k in d:
        print(str(k), str(lookup(d, k)))
show({"a": 1, 2: "b"})
