def keys(q: unknown) -> str:
    out = ""
    if isinstance(q, dict):
        for k in q:
            out = out + str(k) + ","
    return out

def items(q: unknown) -> str:
    out = ""
    if isinstance(q, dict):
        for k in q:
            out = out + str(k) + ","
        for it in q.items():
            out = out + str(it[0]) + ":" + str(it[1]) + ";"
    return out

def getv(q: unknown) -> str:
    out = ""
    if isinstance(q, dict):
        for k in q.keys():
            out = out + str(k) + "->" + str(q.get(k, "?")) + ","
    return out

print(keys({"a": 1, "b": 2}))
print(items({"a": 1, "b": 2}))
print(getv({"a": 1}))
