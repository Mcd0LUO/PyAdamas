def erase(v: big) -> object:
    return v

a: big = 18446744073709551623
b: big = a + 1 - 1
x: object = erase(a)
y: object = erase(b)
d: dict[object, int] = {}
d[x] = 41
print(y in d, d.get(y, 0), len(d))
s: set[object] = set()
s.add(x)
print(y in s)
# --- expect ---
# True 41 1
# True
