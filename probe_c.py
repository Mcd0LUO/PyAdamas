import math
print(math.frexp(8.0))
print(math.ldexp(0.5, 4))
print(math.isinf(1e308 * 10))
print(math.isnan(0.0 / 0.0))

b = bytes([0xc3, 0xa9])
print(b.decode("utf-8"))
print(b.decode("utf-8", "replace"))
print(bytes([0xff]).decode("utf-8", "replace"))

def mk(xs: list[unknown]) -> tuple[unknown, ...]:
    return tuple(xs)

a, c = mk([1, 2])
print(a, c)
print(a + c)

def is_list(x) -> bool:
    return isinstance(x, list)

print(is_list([1]), is_list(3))

print("%d %s %03d" % (1, "x", 7))
print(1 << 63, (1 << 63) - 1 + (1 << 63))
print(int(2.9), float(3))
print(chr(200), ord("é"))
print("é".encode())
print(len("é".encode()))
