# pyadamas: type-tags
# Also run with tags disabled for the probe-only path.
def put(v: big) -> object:
    return v

def get(v: object) -> big:
    x: big = v
    return x

a: big = 123456789012345678901234567890
z: big = 0
v: object = put(a + 3)
w: object = put(a + 3)
print(v, str(v), repr(v), get(v))
print(v == w, v > a, v + a, 2 + v, v * -2, -v, v // 2, v % 2)
print(get(5), get(8), bool(put(z)))
print(isinstance(v, int), isinstance(v, float))
items: object = [v, w]
print(repr(items), items)
print(v < float("inf"), v != float("nan"))
try:
    print(v // 0)
except ZeroDivisionError:
    print("caught dynamic division")
# --- expect ---
# 123456789012345678901234567893 123456789012345678901234567893 123456789012345678901234567893 123456789012345678901234567893
# True True 246913578024691357802469135783 123456789012345678901234567895 -246913578024691357802469135786 -123456789012345678901234567893 61728394506172839450617283946 1
# 5 8 False
# True False
# [123456789012345678901234567893, 123456789012345678901234567893] [123456789012345678901234567893, 123456789012345678901234567893]
# True True
# caught dynamic division
