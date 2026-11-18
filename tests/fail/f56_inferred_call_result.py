# expect: argument 'xs' of 'consume' expects 'list[str]', got 'list[int]'
def make(value):
    return [value]


def consume(xs: list[str]):
    return len(xs)


consume(make(1))
