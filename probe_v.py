def show(x: unknown) -> None:
    print(isinstance(x, str), isinstance(x, bytes), isinstance(x, int), isinstance(x, list))

show("a")
show(b"a")
show(1)
show([1])

def mixed(a: unknown, b: unknown) -> str:
    a_is_str = isinstance(a, str)
    a_is_bytes = isinstance(a, bytes)
    b_is_str = isinstance(b, str)
    b_is_bytes = isinstance(b, bytes)
    print(a_is_str, a_is_bytes, b_is_str, b_is_bytes)
    if a_is_str != b_is_str:
        return "mismatch"
    return "same"

print(mixed("a", b"a"))
print(mixed("a", "b"))
print(mixed(b"a", b"b"))
