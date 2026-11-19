# join 异常传播：vthread 内逃出的异常在 join 处重抛
def bad(n):
    if n > 0:
        raise ValueError("boom")

h = go bad(3)
try:
    join(h)
except ValueError:
    print("caught value")
print("after")

def inner():
    raise KeyError("k")

def outer():
    inner()

h2 = go outer()
try:
    join(h2)
except KeyError:
    print("caught key")

def selfcatch():
    try:
        raise RuntimeError("x")
    except RuntimeError:
        print("self caught")

h3 = go selfcatch()
join(h3)
print("done")
# --- expect ---
# caught value
# after
# caught key
# self caught
# done
