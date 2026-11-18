# expect: argument 'flag' of 'consume' expects 'bool', got 'int'
def make() -> tuple[int, list]:
    return (True, [1.25])


def consume(flag: bool):
    return flag


consume(make()[0])
