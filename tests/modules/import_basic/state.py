counter = 0
value = 1


def bump() -> int:
    global counter
    counter = counter + 1
    return counter
