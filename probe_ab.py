def show(x: unknown) -> None:
    print(repr(x))

show(1)
show(1.5)
show(True)
show(False)
show("s")
show(None)
show([1, "a", None, True])
show((1, 2))
show({"a": 1})
show({1, 2})
show(b"xy")
