g = 0

def f(n: int, x: float) -> int:
    global g
    i = 0
    while i < n:
        g = g + 1
        i = i + 1
        y = int(x + float(i))
    a = 0
    while a < 3:
        a = a + 1
    b = 0
    while b < 3:
        b = b + 1
    c = 0
    while c < 3:
        c = c + 1
    d = 0
    while d < 3:
        d = d + 1
    return i + a + b + c + d

def try_it() -> int:
    try:
        f(5, float("inf"))
    except OverflowError:
        return 1
    return 0

r = try_it()
print(r)
print(g)

# --- expect ---
# 1
# 1
