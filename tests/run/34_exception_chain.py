class MyError(ValueError):
    pass


class SubError(MyError):
    pass


def f() -> int:
    raise SubError("sub")


try:
    f()
except MyError as e:
    print("caught my", e)
except Exception:
    print("wrong")

try:
    f()
except ValueError as e:
    print("as valueerror", e)

try:
    try:
        f()
    except SubError as e:
        raise RuntimeError("wrap") from e
except RuntimeError as e:
    print("wrapped", e)
    c = e.__cause__
    if c is None:
        print("no cause")
    else:
        print("cause", c)

try:
    try:
        f()
    except SubError:
        raise MyError from None
except MyError as e:
    print("noca", len(e.args))
    c2 = e.__cause__
    if c2 is None:
        print("cause none")
    else:
        print("unexpected cause")

# --- expect ---
# caught my sub
# as valueerror sub
# wrapped wrap
# cause sub
# noca 1
# cause none
