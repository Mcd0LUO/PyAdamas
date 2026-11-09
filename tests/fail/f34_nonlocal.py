# expect: no binding for nonlocal 'x' found
def f() -> None:
    nonlocal x
