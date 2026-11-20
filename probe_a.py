b = bytes([0xff, 0x00, 0x41, 128])
print(len(b), b[0], b[3])
print(b[2])
print(b[1:3].decode("latin-1"))

xs: list[unknown] = []
xs.append(1)
xs.append("s")
xs.append(3.5)
t = tuple(xs)
print(t)
print(len(t), t[0], t[1])

d = {"a": 1, "b": 2}
print(sorted(d.keys(), reverse=True))
print(list(d.values()))

s = "Hello World"
print(s.split(" "), s.lower(), s.find("World"), s.startswith("He"))
print(int("ff", 16), int("10", 16))

class MyError(Exception):
    """x"""

def boom() -> None:
    raise MyError("bad")

try:
    boom()
except MyError as e:
    print("caught", e)
print("done")
