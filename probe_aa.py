def show(x: unknown) -> None:
    print(str(x), "|", isinstance(x, list), isinstance(x, tuple), isinstance(x, dict), isinstance(x, set), isinstance(x, str), isinstance(x, int), isinstance(x, float), isinstance(x, bool))

show(1)
show(1.5)
show(True)
show("s")
show([1, 2])
show((1, 2))
show({"a": 1})
show({1, 2})
show(None)
show(set())
