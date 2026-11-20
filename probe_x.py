def name(x: unknown) -> str:
    return type(x).__name__

print(name(1))
print(name("a"))
print(name(b"a"))
print(name([1]))
print(name({"a": 1}))
print(name((1, 2)))
print(name(1.5))
print(name(None))
