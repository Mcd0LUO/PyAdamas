def to_list(x: unknown) -> list[unknown]:
    return list(x)

def chk(d: dict[unknown, list[int]], k: unknown) -> bool:
    return k in d

def setv(d: dict[unknown, list[int]], k: unknown, v: list[int]) -> None:
    d[k] = v

print(to_list("abc"))
print(to_list([1, 2]))
d: dict[unknown, list[int]] = {}
print(chk(d, "a"))
setv(d, "a", [1])
print(chk(d, "a"))
print(len(d))
for k in d:
    print(str(k), str(d.get("a")))
