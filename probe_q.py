def enc(q: unknown) -> str:
    out = ""
    if isinstance(q, dict):
        for k in q:
            out = out + str(k) + "=" + str(q[k]) + "&"
        return out
    for p in q:
        out = out + str(p[0]) + "=" + str(p[1]) + "&"
    return out

print(enc({"a": "1", "b": "2"}))
print(enc([("a", "1"), ("b", "2")]))
print(enc([["a", "1"]]))

def opt(x: unknown) -> str:
    if x is None:
        return "none"
    if not x:
        return "empty"
    return "some:" + str(x)

print(opt(None), opt(""), opt(0), opt("s"))
d = {"a": 1}
print(d.get("b"), d.get("b", 0))
