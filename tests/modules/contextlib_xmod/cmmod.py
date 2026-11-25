from contextlib import contextmanager, nullcontext


@contextmanager
def managed(tag: str):
    print('enter', tag)
    yield 1
    print('exit', tag)


def make_null(v: int) -> nullcontext:
    return nullcontext(v)
