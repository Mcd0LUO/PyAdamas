def show(x: unknown) -> None:
    print(x is None, x is True, x is False, x == 1, x == 0)

show(None)
show(0)
show(1)
show(True)
show(False)
show("s")
